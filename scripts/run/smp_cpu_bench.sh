#!/usr/bin/env bash
# F7LY 双架构 KVM SMP CPU 压测与亲和性验证脚本。
#
# 用法示例：
#   # 分别在 riscv64 与 loongarch64 原生宿主机执行：
#   scripts/run/smp_cpu_bench.sh --arch rv
#   scripts/run/smp_cpu_bench.sh --arch la
#   scripts/run/smp_cpu_bench.sh --arch rv --worker-list 1,2 --runs 3 --seconds 5
#   scripts/run/smp_cpu_bench.sh --arch all --worker-list 1,2 --seconds 10
#
# 脚本为 RISC-V 和 LoongArch 分别静态编译 tools/smp/f7ly_smp_cpu_bench.c，
# RV 使用 make shell 的 rootfs-riscv64.img，LA 使用现有评测 sdcard；复制到 /tmp
# 后注入该程序。每个 pthread worker 固定绑定一个 CPU，
# 负载期间反复采样 getcpu(2)，仅在“起止 CPU 与全部采样均为目标 CPU”时通过。
# 其负载语义对齐 sysbench cpu 的重复素数计算；因为当前 rv64/la64 Alpine 包仓
# 没有可直接安装的 sysbench 包，采用此可审计、无网络依赖的静态替代程序。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly BENCH_SOURCE="${PROJECT_ROOT}/tools/smp/f7ly_smp_cpu_bench.c"
readonly BENCH_DIR="${PROJECT_ROOT}/build/smp-bench"
readonly LOG_DIR="${PROJECT_ROOT}/logs/run"

arch_selection="all"
worker_list="1,2,4,8"
seconds=3
max_prime=2000
runs=3
min_parallel_speedup="1.10"
qemu_mem="8G"
# 缩放比较必须在同一台多核虚拟机上完成；默认固定 8 vCPU，不能让基线
# worker=1 同时退化成 QEMU 单核而把 CPU 数变化混入吞吐结果。
qemu_cpus=8
temporary_images=()
declare -A prepared_images=()

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    local image
    # 只清理本脚本自己在 /tmp 创建的临时 rootfs，绝不触碰 images/ 下的原始镜像。
    for image in "${temporary_images[@]}"; do
        if [[ "${image}" == /tmp/f7ly-smp-cpu-* ]]; then
            rm -f -- "${image}"
        fi
    done
}
trap cleanup EXIT

is_positive_integer() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

usage() {
    cat <<'EOF'
用法：scripts/run/smp_cpu_bench.sh [选项]

选项：
  --arch rv|la|all          要验证的架构，默认 all
  --worker-list 1,2,4,8     逗号分隔的 worker 数量，默认 1,2,4,8
  --runs N                   每种 worker 配置运行次数，默认 3
  --seconds N               每个 worker 的持续时间，默认 3
  --max-prime N             每轮计算的最大素数，默认 2000
  --qemu-cpus N             QEMU vCPU 数；默认 8，最大 8
  --min-speedup X           4/8 worker 中位吞吐相对 1 worker 的门槛，默认 1.10
  --qemu-mem SIZE           QEMU 内存，默认 8G
  -h, --help                显示本说明

说明：
  性能缩放验收禁止回退到 TCG。rv 必须在 riscv64 KVM 宿主运行，la 必须在
  loongarch64 KVM 宿主运行；官方跨架构 QEMU 回归与本脚本的硬件并行验收分开执行。
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            (($# >= 2)) || die "--arch 缺少参数"
            arch_selection="$2"
            shift 2
            ;;
        --worker-list)
            (($# >= 2)) || die "--worker-list 缺少参数"
            worker_list="$2"
            shift 2
            ;;
        --seconds)
            (($# >= 2)) || die "--seconds 缺少参数"
            seconds="$2"
            shift 2
            ;;
        --runs)
            (($# >= 2)) || die "--runs 缺少参数"
            runs="$2"
            shift 2
            ;;
        --max-prime)
            (($# >= 2)) || die "--max-prime 缺少参数"
            max_prime="$2"
            shift 2
            ;;
        --qemu-cpus)
            (($# >= 2)) || die "--qemu-cpus 缺少参数"
            qemu_cpus="$2"
            shift 2
            ;;
        --qemu-mem)
            (($# >= 2)) || die "--qemu-mem 缺少参数"
            qemu_mem="$2"
            shift 2
            ;;
        --min-speedup)
            (($# >= 2)) || die "--min-speedup 缺少参数"
            min_parallel_speedup="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "未知参数：$1"
            ;;
    esac
done

case "${arch_selection}" in
    rv|la|all) ;;
    *) die "--arch 只能是 rv、la 或 all" ;;
esac
is_positive_integer "${seconds}" || die "--seconds 必须是正整数"
is_positive_integer "${runs}" || die "--runs 必须是正整数"
is_positive_integer "${max_prime}" || die "--max-prime 必须是正整数"
[[ "${min_parallel_speedup}" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
    die "--min-speedup 必须是非负数"
is_positive_integer "${qemu_cpus}" || die "--qemu-cpus 必须是正整数"
((qemu_cpus >= 2)) || die "SMP 缩放验证要求 --qemu-cpus 至少为 2"
((qemu_cpus <= 8)) || die "--qemu-cpus 不能超过内核 NCPU=8：${qemu_cpus}"

IFS=',' read -r -a worker_counts <<< "${worker_list}"
((${#worker_counts[@]} > 0)) || die "--worker-list 不能为空"
has_baseline=0
has_parallel=0
for worker_count in "${worker_counts[@]}"; do
    is_positive_integer "${worker_count}" || die "worker 数必须是正整数：${worker_count}"
    ((worker_count <= 8)) || die "worker 数不能超过内核 NCPU=8：${worker_count}"
    ((worker_count <= qemu_cpus)) || die "worker 数不能超过 --qemu-cpus：${worker_count} > ${qemu_cpus}"
    if ((worker_count == 1)); then
        has_baseline=1
    else
        has_parallel=1
    fi
done
((has_baseline == 1)) || die "--worker-list 必须包含 1，才能计算 speedup"
((has_parallel == 1)) || die "--worker-list 至少要包含一个大于 1 的 worker 数"

for command_name in debugfs lscpu rg uname; do
    command -v "${command_name}" >/dev/null ||
        die "缺少命令：${command_name}"
done
host_physical_cores="$(lscpu -p=CORE,SOCKET |
    awk -F, '!/^#/ { key=$1 FS $2; seen[key]=1 } END { print length(seen) }')"
((host_physical_cores >= qemu_cpus)) ||
    die "决赛要求至少 ${qemu_cpus} 个物理核，当前仅探测到 ${host_physical_cores} 个"

preflight_kvm_arch() {
    local arch="$1"
    local required_host_arch qemu_bin

    case "${arch}" in
        rv)
            required_host_arch="riscv64"
            qemu_bin="qemu-system-riscv64"
            ;;
        la)
            required_host_arch="loongarch64"
            qemu_bin="qemu-system-loongarch64"
            ;;
        *)
            die "内部错误：未知架构 ${arch}"
            ;;
    esac

    command -v "${qemu_bin}" >/dev/null || die "缺少 QEMU：${qemu_bin}"
    [[ "$(uname -m)" == "${required_host_arch}" ]] ||
        die "${arch} SMP 性能验收必须在 ${required_host_arch} 原生宿主使用 KVM；当前宿主为 $(uname -m)"
    [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] ||
        die "${arch} SMP 性能验收需要当前用户可读写 /dev/kvm"
    "${qemu_bin}" -accel help 2>&1 |
        rg -q '(^|[[:space:]])kvm($|[[:space:]])' ||
        die "${qemu_bin} 未提供 KVM accelerator"
}

if [[ "${arch_selection}" == rv || "${arch_selection}" == all ]]; then
    preflight_kvm_arch rv
fi
if [[ "${arch_selection}" == la || "${arch_selection}" == all ]]; then
    preflight_kvm_arch la
fi

mkdir -p "${BENCH_DIR}" "${LOG_DIR}"
run_timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="${LOG_DIR}/smp-cpu-${arch_selection}-smp${qemu_cpus}-${run_timestamp}"
metrics_tsv="${result_dir}/metrics.tsv"
summary_tsv="${result_dir}/summary.tsv"
mkdir -p "${result_dir}"
printf 'arch\tworkers\trun\tevents\telapsed_seconds\tevents_per_second\tstatus\tlog_file\n' >"${metrics_tsv}"

build_and_run_case() {
    local arch="$1"
    local workers="$2"
    local qemu_cpu_count="$3"
    local run_index="$4"
    local compiler kernel_arch kernel_image rootfs_image qemu_bin benchmark_binary
    local timestamp temporary_rootfs log_file command_line timeout_seconds qemu_exit_code
    local metric_line metric_workers metric_events metric_elapsed metric_rate metric_status

    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            kernel_arch="riscv"
            kernel_image="${PROJECT_ROOT}/kernel-rv-shell"
            rootfs_image="${PROJECT_ROOT}/images/sdcard-rv-pub.img"
            qemu_bin="qemu-system-riscv64"
            benchmark_binary="${BENCH_DIR}/f7ly_smp_cpu_bench-rv"
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            kernel_arch="loongarch"
            kernel_image="${PROJECT_ROOT}/kernel-la-shell"
            rootfs_image="${PROJECT_ROOT}/images/sdcard-la-pub.img"
            qemu_bin="qemu-system-loongarch64"
            benchmark_binary="${BENCH_DIR}/f7ly_smp_cpu_bench-la"
            ;;
        *)
            die "内部错误：未知架构 ${arch}"
            ;;
    esac

    command -v "${compiler}" >/dev/null || die "缺少交叉编译器：${compiler}"
    command -v "${qemu_bin}" >/dev/null || die "缺少 QEMU：${qemu_bin}"
    [[ -f "${rootfs_image}" ]] || die "缺少 rootfs：${rootfs_image}"
    [[ -f "${BENCH_SOURCE}" ]] || die "缺少压测源码：${BENCH_SOURCE}"

    if [[ -z "${prepared_images[${arch}]:-}" ]]; then
        echo "[SMP] 构建 ${arch} shell 内核和静态压测器"
        if ! make -C "${PROJECT_ROOT}" build ARCH="${kernel_arch}" INITCODE_MODE=shell; then
            return 1
        fi
        if ! "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
            "${BENCH_SOURCE}" -o "${benchmark_binary}"; then
            return 1
        fi

        if ! temporary_rootfs="$(mktemp /tmp/f7ly-smp-cpu-${arch}-XXXXXX.img)"; then
            return 1
        fi
        temporary_images+=("${temporary_rootfs}")
        if ! cp --reflink=auto --sparse=always "${rootfs_image}" "${temporary_rootfs}"; then
            return 1
        fi
        debugfs -w -R 'rm /f7ly_smp_cpu_bench' "${temporary_rootfs}" >/dev/null 2>&1 || true
        if ! debugfs -w -R "write ${benchmark_binary} /f7ly_smp_cpu_bench" "${temporary_rootfs}" >/dev/null; then
            return 1
        fi
        debugfs -w -R 'set_inode_field /f7ly_smp_cpu_bench mode 0100755' \
            "${temporary_rootfs}" >/dev/null
        prepared_images["${arch}"]="${temporary_rootfs}"
    fi
    temporary_rootfs="${prepared_images[${arch}]}"

    timestamp="$(date +%Y%m%d-%H%M%S)"
    log_file="${result_dir}/output_${arch}_smp${qemu_cpu_count}_workers${workers}_run${run_index}_cpu_bench_${timestamp}.txt"
    command_line="/f7ly_smp_cpu_bench --workers ${workers} --seconds ${seconds} --max-prime ${max_prime}"
    timeout_seconds=$((seconds + 90))
    echo "[SMP] 运行 ${arch}，vCPU=${qemu_cpu_count}，workers=${workers}，第 ${run_index}/${runs} 轮，日志：${log_file}"

    set +e
    if [[ "${arch}" == "rv" ]]; then
        { sleep 3; printf '%s\n' "${command_line}" 'exit'; } |
            timeout "${timeout_seconds}s" "${qemu_bin}" \
                -machine virt -accel kvm -kernel "${kernel_image}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpu_count}" -bios default \
                -snapshot \
                -drive file="${temporary_rootfs}",if=none,format=raw,id=x0 \
                -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
                -device virtio-net-device,netdev=net -netdev user,id=net \
                -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log_file}" 2>&1
        qemu_exit_code=${PIPESTATUS[1]}
    else
        { sleep 3; printf '%s\n' "${command_line}" 'exit'; } |
            timeout "${timeout_seconds}s" "${qemu_bin}" \
                -machine virt -accel kvm -kernel "${kernel_image}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpu_count}" \
                -snapshot \
                -drive file="${temporary_rootfs}",if=none,format=raw,id=x0 \
                -device virtio-blk-pci,drive=x0 \
                -netdev user,id=net -device virtio-net-pci,netdev=net \
                -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log_file}" 2>&1
        qemu_exit_code=${PIPESTATUS[1]}
    fi
    set -e

    if ((qemu_exit_code != 0)); then
        echo "[SMP] ${arch} vCPU=${qemu_cpu_count} workers=${workers} 的 QEMU 退出码为 ${qemu_exit_code}，见：${log_file}" >&2
        return 1
    fi
    if ! rg -q '^F7LY_SMP_CPU_PASS$' "${log_file}"; then
        echo "[SMP] ${arch} vCPU=${qemu_cpu_count} workers=${workers} 未出现 PASS，见：${log_file}" >&2
        rg -n 'F7LY_SMP_CPU_|PANIC|panic|page fault|unexpected scause' "${log_file}" || true
        return 1
    fi
    if [[ "$(rg -c '^F7LY_SMP_CPU_WORKER .* status=PASS$' "${log_file}")" -ne "${workers}" ]]; then
        echo "[SMP] ${arch} vCPU=${qemu_cpu_count} workers=${workers} 的 worker 亲和性记录不完整，见：${log_file}" >&2
        rg -n '^F7LY_SMP_CPU_WORKER' "${log_file}" || true
        return 1
    fi

    # 把 guest 自己按墙钟时间计算的 events/sec 提取到结构化结果中。这里
    # 不用 host 的 QEMU 运行时长，避免启动延迟、串口阻塞或 TCG 抖动污染
    # sysbench 风格的 CPU 吞吐指标。
    metric_line="$(rg '^F7LY_SMP_CPU_METRICS ' "${log_file}" | tail -n 1 || true)"
    if [[ -z "${metric_line}" ]]; then
        echo "[SMP] ${arch} workers=${workers} 缺少 guest 吞吐指标，见：${log_file}" >&2
        return 1
    fi
    read -r metric_workers metric_events metric_elapsed metric_rate metric_status < <(
        awk '
        {
            for (field_index = 1; field_index <= NF; ++field_index) {
                split($field_index, pair, "=")
                if (pair[1] == "workers") workers = pair[2]
                else if (pair[1] == "events") events = pair[2]
                else if (pair[1] == "elapsed_seconds") elapsed = pair[2]
                else if (pair[1] == "events_per_second") rate = pair[2]
                else if (pair[1] == "status") status = pair[2]
            }
        }
        END { print workers, events, elapsed, rate, status }
        ' <<<"${metric_line}"
    )
    if [[ "${metric_workers}" != "${workers}" || "${metric_status}" != "PASS" ||
          -z "${metric_events}" || -z "${metric_elapsed}" || -z "${metric_rate}" ]]; then
        echo "[SMP] ${arch} workers=${workers} 的 guest 吞吐指标无效：${metric_line}" >&2
        return 1
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${arch}" "${metric_workers}" "${run_index}" "${metric_events}" \
        "${metric_elapsed}" "${metric_rate}" "${metric_status}" "${log_file}" >>"${metrics_tsv}"

    echo "[SMP] PASS ${arch} vCPU=${qemu_cpu_count} workers=${workers}：${log_file}"
}

selected_arches=()
case "${arch_selection}" in
    rv) selected_arches=(rv) ;;
    la) selected_arches=(la) ;;
    all) selected_arches=(rv la) ;;
esac

failed=0
for arch in "${selected_arches[@]}"; do
    for worker_count in "${worker_counts[@]}"; do
        for ((run_index = 1; run_index <= runs; ++run_index)); do
            if ! build_and_run_case "${arch}" "${worker_count}" "${qemu_cpus}" "${run_index}"; then
                failed=1
            fi
        done
    done
done

# 以三轮中位数计算吞吐、加速比和并行效率，避免单轮宿主负载抖动左右结论。
awk -F '\t' -v OFS='\t' -v worker_order="${worker_list}" -v arch_order="${arch_selection}" '
NR == 1 { next }
$7 == "PASS" {
    key = $1 SUBSEP $2
    count[key]++
    rate[key SUBSEP count[key]] = $6 + 0
}
function median(key, count_value, values, i, j, current) {
    for (i = 1; i <= count_value; ++i)
        values[i] = rate[key SUBSEP i]
    for (i = 2; i <= count_value; ++i) {
        current = values[i]
        j = i - 1
        while (j >= 1 && values[j] > current) {
            values[j + 1] = values[j]
            --j
        }
        values[j + 1] = current
    }
    if (count_value % 2 == 1)
        current = values[(count_value + 1) / 2]
    else
        current = (values[count_value / 2] + values[count_value / 2 + 1]) / 2
    for (i = 1; i <= count_value; ++i)
        delete values[i]
    return current
}
END {
    print "arch", "workers", "successful_runs", "median_events_per_second", "speedup", "efficiency_percent"
    if (arch_order == "all") {
        arch_count = split("rv,la", arches, ",")
    } else {
        arch_count = split(arch_order, arches, ",")
    }
    worker_count = split(worker_order, workers, ",")
    for (a = 1; a <= arch_count; ++a) {
        arch = arches[a]
        baseline_key = arch SUBSEP 1
        baseline = count[baseline_key] ? median(baseline_key, count[baseline_key]) : 0
        for (w = 1; w <= worker_count; ++w) {
            worker = workers[w]
            key = arch SUBSEP worker
            middle = count[key] ? median(key, count[key]) : 0
            speedup = baseline > 0 ? middle / baseline : 0
            efficiency = worker > 0 ? speedup / worker * 100 : 0
            printf "%s\t%d\t%d\t%.3f\t%.3f\t%.2f\n", \
                   arch, worker, count[key] + 0, middle, speedup, efficiency
        }
    }
}
' "${metrics_tsv}" >"${summary_tsv}"

echo "[SMP] 逐项指标：${metrics_tsv}"
echo "[SMP] 缩放汇总：${summary_tsv}"
column -t -s $'\t' "${summary_tsv}" 2>/dev/null || cat "${summary_tsv}"

for arch in "${selected_arches[@]}"; do
    baseline_count="$(awk -F '\t' -v arch="${arch}" '$1 == arch && $2 == 1 { print $3; exit }' "${summary_tsv}")"
    if [[ "${baseline_count:-0}" -lt "${runs}" ]]; then
        echo "[SMP] ${arch} worker=1 有效轮次不足：${baseline_count:-0}/${runs}" >&2
        failed=1
    fi
    largest_parallel_worker=0
    trend_workers=()
    for worker_count in "${worker_counts[@]}"; do
        if ((worker_count > largest_parallel_worker)); then
            largest_parallel_worker=${worker_count}
        fi
        if ((worker_count == 4 || worker_count == 8)); then
            trend_workers+=("${worker_count}")
        fi
    done
    if ((${#trend_workers[@]} == 0)); then
        trend_workers+=("${largest_parallel_worker}")
    fi
    for worker_count in "${trend_workers[@]}"; do
        if ! awk -F '\t' -v arch="${arch}" -v worker="${worker_count}" \
            -v threshold="${min_parallel_speedup}" -v required_runs="${runs}" \
            '$1 == arch && $2 == worker && $3 >= required_runs && ($5 + 0) >= threshold { pass = 1 }
             END { exit !pass }' "${summary_tsv}"; then
            echo "[SMP] ${arch} workers=${worker_count} 中位吞吐未达到 ${min_parallel_speedup} 倍门槛" >&2
            failed=1
        fi
    done
done

if ((failed != 0)); then
    echo "[SMP] 存在失败项，请按日志定位。" >&2
    exit 1
fi
echo "[SMP] 全部架构与 worker 配置通过。"

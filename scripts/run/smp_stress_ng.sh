#!/usr/bin/env bash
# 使用 RISC-V shell rootfs 内真实 stress-ng 验证 F7LY 的多核 CPU 利用率。
#
# 用法示例：
#   scripts/run/smp_stress_ng.sh
#   scripts/run/smp_stress_ng.sh --seconds 3 --runs 1 --warmup-seconds 1
#   scripts/run/smp_stress_ng.sh --qemu-cpus 8 --worker-list 1,2,4,8 --seconds 10 --runs 3
#
# 脚本只读使用 images/rootfs-riscv64.img，并通过 QEMU -snapshot 丢弃 guest 写入。
# 每次执行保留完整串口日志、逐轮指标和汇总结果，不以“线程创建成功”替代吞吐验证。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly LOG_ROOT="${PROJECT_ROOT}/logs/run"

qemu_cpus=8
worker_list="1,2,4,8"
seconds=10
runs=3
warmup_seconds=2
boot_wait_seconds=4
qemu_mem="1G"
rootfs="${PROJECT_ROOT}/images/rootfs-riscv64.img"
skip_build=0
guest_commands=""

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    if [[ -n "${guest_commands}" && "${guest_commands}" == /tmp/f7ly-stress-ng-* ]]; then
        rm -f -- "${guest_commands}"
    fi
}
trap cleanup EXIT

is_non_negative_integer() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

is_positive_integer() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

usage() {
    cat <<'EOF'
用法：scripts/run/smp_stress_ng.sh [选项]

选项：
  --qemu-cpus N             QEMU vCPU 数，默认 8，最大 8
  --worker-list 1,2,4,8     stress-ng CPU worker 矩阵，必须包含 1
  --seconds N               每轮计量时长，默认 10 秒
  --runs N                  每个 worker 的计量轮数，默认 3
  --warmup-seconds N        每个 worker 的预热时长，0 表示关闭，默认 2
  --boot-wait-seconds N     注入命令前的等待时间，默认 4 秒
  --qemu-mem SIZE           QEMU 内存，默认 1G
  --rootfs PATH             含真实 stress-ng 的 RV64 rootfs
  --skip-build              复用已有 kernel-rv-shell
  -h, --help                显示本说明
EOF
}

while (($# > 0)); do
    case "$1" in
        --qemu-cpus)
            (($# >= 2)) || die "--qemu-cpus 缺少参数"
            qemu_cpus="$2"
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
        --warmup-seconds)
            (($# >= 2)) || die "--warmup-seconds 缺少参数"
            warmup_seconds="$2"
            shift 2
            ;;
        --boot-wait-seconds)
            (($# >= 2)) || die "--boot-wait-seconds 缺少参数"
            boot_wait_seconds="$2"
            shift 2
            ;;
        --qemu-mem)
            (($# >= 2)) || die "--qemu-mem 缺少参数"
            qemu_mem="$2"
            shift 2
            ;;
        --rootfs)
            (($# >= 2)) || die "--rootfs 缺少参数"
            rootfs="$2"
            shift 2
            ;;
        --skip-build)
            skip_build=1
            shift
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

is_positive_integer "${qemu_cpus}" || die "--qemu-cpus 必须是正整数"
((qemu_cpus >= 2)) || die "多核验证要求 --qemu-cpus 至少为 2"
((qemu_cpus <= 8)) || die "--qemu-cpus 不能超过内核 NCPU=8"
is_positive_integer "${seconds}" || die "--seconds 必须是正整数"
is_positive_integer "${runs}" || die "--runs 必须是正整数"
is_non_negative_integer "${warmup_seconds}" || die "--warmup-seconds 必须是非负整数"
is_non_negative_integer "${boot_wait_seconds}" || die "--boot-wait-seconds 必须是非负整数"

IFS=',' read -r -a worker_counts <<< "${worker_list}"
((${#worker_counts[@]} > 0)) || die "--worker-list 不能为空"
declare -A seen_workers=()
has_baseline=0
has_parallel=0
for worker_count in "${worker_counts[@]}"; do
    is_positive_integer "${worker_count}" || die "worker 数必须是正整数：${worker_count}"
    ((worker_count <= qemu_cpus)) || die "worker 数不能超过 QEMU vCPU：${worker_count} > ${qemu_cpus}"
    [[ -z "${seen_workers[${worker_count}]:-}" ]] || die "worker 数不能重复：${worker_count}"
    seen_workers[${worker_count}]=1
    if ((worker_count == 1)); then
        has_baseline=1
    else
        has_parallel=1
    fi
done
((has_baseline == 1)) || die "--worker-list 必须包含 1，才能计算加速比"
((has_parallel == 1)) || die "--worker-list 至少要包含一个大于 1 的 worker 数"

command -v qemu-system-riscv64 >/dev/null || die "缺少 qemu-system-riscv64"
command -v rg >/dev/null || die "缺少 rg"
[[ -f "${rootfs}" ]] || die "缺少 rootfs：${rootfs}"

if ((skip_build == 0)); then
    echo "[stress-ng] 构建 RISC-V shell 内核"
    make -C "${PROJECT_ROOT}" build ARCH=riscv INITCODE_MODE=shell
fi
readonly kernel_image="${PROJECT_ROOT}/kernel-rv-shell"
[[ -x "${kernel_image}" ]] || die "缺少 shell 内核：${kernel_image}"

timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="${LOG_ROOT}/stress-ng-rv-smp${qemu_cpus}-${timestamp}"
console_log="${result_dir}/console.log"
metrics_tsv="${result_dir}/metrics.tsv"
summary_tsv="${result_dir}/summary.tsv"
run_env="${result_dir}/run.env"
mkdir -p "${result_dir}"
guest_commands="$(mktemp /tmp/f7ly-stress-ng-commands-XXXXXX)"

# 每条命令用唯一标记包围。只解析标记之间的 stress-ng CPU 指标，避免启动日志
# 或 shell 命令回显被误判为一次有效计量。
{
    echo 'echo __F7LY_STRESS_SESSION_BEGIN__'
    echo 'if test -x /usr/bin/stress-ng; then echo __F7LY_STRESS_BINARY_OK__; else echo __F7LY_STRESS_BINARY_MISSING__; fi'
    echo 'cpu_count=$(/usr/bin/nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || grep -c "^processor" /proc/cpuinfo 2>/dev/null || echo 0); echo __F7LY_CPU_COUNT__ count=${cpu_count}; echo __F7LY_CPUINFO_BEGIN__; cat /sys/devices/system/cpu/online 2>/dev/null || true; echo __F7LY_CPUINFO_END__'
    for worker_count in "${worker_counts[@]}"; do
        if ((warmup_seconds > 0)); then
            printf 'echo __F7LY_STRESS_WARMUP_BEGIN__ workers=%d; /usr/bin/stress-ng --cpu %d --timeout %ds --metrics-brief >/dev/null 2>&1; rc=$?; echo __F7LY_STRESS_WARMUP_END__ workers=%d rc=${rc}\n' \
                "${worker_count}" "${worker_count}" "${warmup_seconds}" "${worker_count}"
        fi
        for ((run = 1; run <= runs; ++run)); do
            printf 'echo __F7LY_STRESS_BEGIN__ workers=%d run=%d; /usr/bin/stress-ng --cpu %d --timeout %ds --metrics-brief; rc=$?; echo __F7LY_STRESS_END__ workers=%d run=%d rc=${rc}\n' \
                "${worker_count}" "${run}" "${worker_count}" "${seconds}" \
                "${worker_count}" "${run}"
        done
    done
    echo 'echo __F7LY_STRESS_SESSION_END__'
    echo 'exit'
} >"${guest_commands}"

total_stress_seconds=$(((${#worker_counts[@]} * warmup_seconds) + (${#worker_counts[@]} * runs * seconds)))
timeout_seconds=$((boot_wait_seconds + total_stress_seconds + 120))

cat >"${run_env}" <<EOF
arch=riscv64
qemu_cpus=${qemu_cpus}
worker_list=${worker_list}
seconds=${seconds}
runs=${runs}
warmup_seconds=${warmup_seconds}
qemu_mem=${qemu_mem}
rootfs=${rootfs}
kernel=${kernel_image}
timeout_seconds=${timeout_seconds}
EOF

echo "[stress-ng] 启动 RV64：vCPU=${qemu_cpus}，workers=${worker_list}"
echo "[stress-ng] 完整串口日志：${console_log}"
set +e
{ sleep "${boot_wait_seconds}"; sed -n '1,$p' "${guest_commands}"; } |
    timeout "${timeout_seconds}s" qemu-system-riscv64 \
        -machine virt -kernel "${kernel_image}" -m "${qemu_mem}" \
        -display none -chardev stdio,id=shell_stdio,signal=off \
        -serial chardev:shell_stdio -monitor none -smp "${qemu_cpus}" -bios default \
        -snapshot -drive file="${rootfs}",if=none,format=raw,id=x0 \
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
        -device virtio-net-device,netdev=net -netdev user,id=net \
        -no-reboot -rtc base=utc \
        >"${console_log}" 2>&1
qemu_exit_code=${PIPESTATUS[1]}
set -e
echo "qemu_exit_code=${qemu_exit_code}" >>"${run_env}"

# stress-ng 各版本的前缀中 metrc/metric 文案略有差异，但 CPU 数据列稳定为：
# cpu, bogo_ops, real_time, user_time, system_time, bogo_ops_per_second(real)。
awk '
BEGIN {
    OFS = "\t"
    print "workers", "run", "exit_code", "bogo_ops", "real_seconds", "bogo_ops_per_second", "status"
}
{
    sub(/\r$/, "", $0)
}
/^__F7LY_STRESS_BEGIN__ workers=[0-9]+ run=[0-9]+$/ {
    workers = $2
    run = $3
    sub(/^workers=/, "", workers)
    sub(/^run=/, "", run)
    bogo = ""
    real_seconds = ""
    rate = ""
    active = 1
    next
}
active && /stress-ng:.*\][[:space:]]+cpu[[:space:]]+[0-9]/ {
    line = $0
    sub(/^.*\][[:space:]]+cpu[[:space:]]+/, "", line)
    count = split(line, value, /[[:space:]]+/)
    if (count >= 5) {
        bogo = value[1]
        real_seconds = value[2]
        rate = value[5]
    }
    next
}
/^__F7LY_STRESS_END__ workers=[0-9]+ run=[0-9]+ rc=[0-9]+$/ {
    end_workers = $2
    end_run = $3
    exit_code = $4
    sub(/^workers=/, "", end_workers)
    sub(/^run=/, "", end_run)
    sub(/^rc=/, "", exit_code)
    status = (active && workers == end_workers && run == end_run && exit_code == 0 && rate != "") ? "PASS" : "FAIL"
    print end_workers, end_run, exit_code, bogo, real_seconds, rate, status
    active = 0
}
' "${console_log}" >"${metrics_tsv}"

awk -F '\t' -v OFS='\t' -v worker_order="${worker_list}" '
NR == 1 { next }
$7 == "PASS" {
    worker = $1
    value = $6 + 0
    count[worker]++
    sum[worker] += value
    if (!(worker in minimum) || value < minimum[worker]) minimum[worker] = value
    if (!(worker in maximum) || value > maximum[worker]) maximum[worker] = value
}
END {
    print "workers", "successful_runs", "throughput_average", "throughput_min", "throughput_max", "speedup", "efficiency_percent"
    order_count = split(worker_order, order, ",")
    baseline = count[1] ? sum[1] / count[1] : 0
    for (field_index = 1; field_index <= order_count; ++field_index) {
        worker = order[field_index]
        average = count[worker] ? sum[worker] / count[worker] : 0
        speedup = baseline > 0 ? average / baseline : 0
        efficiency = worker > 0 ? speedup / worker * 100 : 0
        printf "%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.2f\n", worker, count[worker], average, minimum[worker] + 0, maximum[worker] + 0, speedup, efficiency
    }
}
' "${metrics_tsv}" >"${summary_tsv}"

expected_runs=$((${#worker_counts[@]} * runs))
successful_runs="$(awk -F '\t' 'NR > 1 && $7 == "PASS" { count++ } END { print count + 0 }' "${metrics_tsv}")"
guest_cpu_count="$(sed -n 's/.*__F7LY_CPU_COUNT__ count=\([0-9][0-9]*\).*/\1/p' "${console_log}" | tail -n 1)"

failed=0
if ((qemu_exit_code != 0)); then
    echo "[stress-ng] QEMU 退出码异常：${qemu_exit_code}" >&2
    failed=1
fi
if ! rg -q '^__F7LY_STRESS_BINARY_OK__$' "${console_log}"; then
    echo "[stress-ng] guest 中未确认 /usr/bin/stress-ng 可执行" >&2
    failed=1
fi
if [[ "${successful_runs}" -ne "${expected_runs}" ]]; then
    echo "[stress-ng] 有效轮次不足：${successful_runs}/${expected_runs}" >&2
    failed=1
fi
if [[ -z "${guest_cpu_count}" ]] || ((guest_cpu_count < qemu_cpus)); then
    echo "[stress-ng] guest 在线 CPU 数不足或无法解析：${guest_cpu_count:-unknown} < ${qemu_cpus}" >&2
    failed=1
fi

# 默认矩阵必须证明 4/8 worker 相对单 worker 有正加速；自定义矩阵若没有
# 4/8，则至少要求其中最大的并行 worker 有正加速。
trend_workers=()
largest_parallel_worker=0
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
    if ! awk -F '\t' -v worker="${worker_count}" '
        NR > 1 && $1 == worker { found = 1; if (($6 + 0) > 1.0) pass = 1 }
        END { exit !(found && pass) }
    ' "${summary_tsv}"; then
        echo "[stress-ng] workers=${worker_count} 未证明相对单 worker 的正加速" >&2
        failed=1
    fi
done
if rg -qi 'panic|unexpected scause|unknown page fault|stress-ng: fail' "${console_log}"; then
    echo "[stress-ng] 串口日志出现内核或 stress-ng 错误关键字" >&2
    failed=1
fi

echo "[stress-ng] 逐轮指标：${metrics_tsv}"
echo "[stress-ng] 汇总结果：${summary_tsv}"
echo "[stress-ng] guest 在线 CPU：${guest_cpu_count:-unknown}/${qemu_cpus}"
column -t -s $'\t' "${summary_tsv}" 2>/dev/null || cat "${summary_tsv}"

if ((failed != 0)); then
    echo "[stress-ng] 验证失败，请检查：${console_log}" >&2
    rg -n 'F7LY_STRESS|stress-ng:|panic|unexpected scause|page fault' "${console_log}" | tail -n 160 || true
    exit 1
fi

echo "[stress-ng] 全部真实 stress-ng 计量轮次通过。"

#!/usr/bin/env bash
# F7LY 双架构 SMP CPU 压测与亲和性验证脚本。
#
# 用法示例：
#   scripts/run/smp_cpu_bench.sh
#   scripts/run/smp_cpu_bench.sh --arch rv --worker-list 1,2 --seconds 5 --max-prime 2000
#   scripts/run/smp_cpu_bench.sh --arch all --worker-list 2 --seconds 10
#
# 脚本为 RISC-V 和 LoongArch 分别静态编译 tools/smp/f7ly_smp_cpu_bench.c，
# 再复制官方 rootfs 到 /tmp 并注入该程序。每个 pthread worker 固定绑定一个 CPU，
# 负载期间反复采样 getcpu(2)，仅在“起止 CPU 与全部采样均为目标 CPU”时通过。
# 其负载语义对齐 sysbench cpu 的重复素数计算；因为当前 rv64/la64 Alpine 包仓
# 没有可直接安装的 sysbench 包，采用此可审计、无网络依赖的静态替代程序。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly BENCH_SOURCE="${PROJECT_ROOT}/tools/smp/f7ly_smp_cpu_bench.c"
readonly BENCH_DIR="${PROJECT_ROOT}/build/smp-bench"
readonly LOG_DIR="${PROJECT_ROOT}/logs/run"

arch_selection="all"
worker_list="1,2"
seconds=3
max_prime=2000
qemu_mem="1G"
qemu_cpus=""
temporary_images=()

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
  --worker-list 1,2         逗号分隔的 worker 数量，默认 1,2
  --seconds N               每个 worker 的持续时间，默认 3
  --max-prime N             每轮计算的最大素数，默认 2000
  --qemu-cpus N             QEMU vCPU 数；默认与当前 worker 数相同
  --qemu-mem SIZE           QEMU 内存，默认 1G
  -h, --help                显示本说明
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
is_positive_integer "${max_prime}" || die "--max-prime 必须是正整数"
if [[ -n "${qemu_cpus}" ]]; then
    is_positive_integer "${qemu_cpus}" || die "--qemu-cpus 必须是正整数"
    ((qemu_cpus <= 8)) || die "--qemu-cpus 不能超过内核 NCPU=8：${qemu_cpus}"
fi

IFS=',' read -r -a worker_counts <<< "${worker_list}"
((${#worker_counts[@]} > 0)) || die "--worker-list 不能为空"
for worker_count in "${worker_counts[@]}"; do
    is_positive_integer "${worker_count}" || die "worker 数必须是正整数：${worker_count}"
    ((worker_count <= 8)) || die "worker 数不能超过内核 NCPU=8：${worker_count}"
    if [[ -n "${qemu_cpus}" ]]; then
        ((worker_count <= qemu_cpus)) || die "worker 数不能超过 --qemu-cpus：${worker_count} > ${qemu_cpus}"
    fi
done

command -v debugfs >/dev/null || die "缺少 debugfs（通常由 e2fsprogs 提供）"
mkdir -p "${BENCH_DIR}" "${LOG_DIR}"

build_and_run_case() {
    local arch="$1"
    local workers="$2"
    local qemu_cpu_count="$3"
    local compiler kernel_arch kernel_image rootfs_image qemu_bin benchmark_binary
    local timestamp temporary_rootfs log_file command_line timeout_seconds qemu_exit_code

    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            kernel_arch="riscv"
            kernel_image="${PROJECT_ROOT}/kernel-rv-shell"
            rootfs_image="${PROJECT_ROOT}/images/sdcard-rv.img"
            qemu_bin="qemu-system-riscv64"
            benchmark_binary="${BENCH_DIR}/f7ly_smp_cpu_bench-rv"
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            kernel_arch="loongarch"
            kernel_image="${PROJECT_ROOT}/kernel-la-shell"
            rootfs_image="${PROJECT_ROOT}/images/sdcard-la.img"
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

    echo "[SMP] 构建 ${arch} shell 内核和静态压测器"
    if ! make -C "${PROJECT_ROOT}" build ARCH="${kernel_arch}" INITCODE_MODE=shell; then
        return 1
    fi
    if ! "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread "${BENCH_SOURCE}" -o "${benchmark_binary}"; then
        return 1
    fi

    timestamp="$(date +%Y%m%d-%H%M%S)"
    if ! temporary_rootfs="$(mktemp /tmp/f7ly-smp-cpu-${arch}-XXXXXX.img)"; then
        return 1
    fi
    temporary_images+=("${temporary_rootfs}")
    if ! cp --reflink=auto "${rootfs_image}" "${temporary_rootfs}"; then
        return 1
    fi
    # fresh copy 上理论上不存在该文件；先删除使脚本能重复用于曾被人工注入的镜像。
    debugfs -w -R 'rm /f7ly_smp_cpu_bench' "${temporary_rootfs}" >/dev/null 2>&1 || true
    if ! debugfs -w -R "write ${benchmark_binary} /f7ly_smp_cpu_bench" "${temporary_rootfs}" >/dev/null; then
        return 1
    fi

    log_file="${LOG_DIR}/output_${arch}_smp${qemu_cpu_count}_workers${workers}_cpu_bench_${timestamp}.txt"
    command_line="/f7ly_smp_cpu_bench --workers ${workers} --seconds ${seconds} --max-prime ${max_prime}"
    timeout_seconds=$((seconds + 90))
    echo "[SMP] 运行 ${arch}，vCPU=${qemu_cpu_count}，workers=${workers}，日志：${log_file}"

    set +e
    if [[ "${arch}" == "rv" ]]; then
        { sleep 3; printf '%s\n' "${command_line}" 'exit'; } |
            timeout "${timeout_seconds}s" "${qemu_bin}" \
                -machine virt -kernel "${kernel_image}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpu_count}" -bios default \
                -drive file="${temporary_rootfs}",if=none,format=raw,id=x0 \
                -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
                -device virtio-net-device,netdev=net -netdev user,id=net \
                -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log_file}" 2>&1
        qemu_exit_code=${PIPESTATUS[1]}
    else
        { sleep 3; printf '%s\n' "${command_line}" 'exit'; } |
            timeout "${timeout_seconds}s" "${qemu_bin}" \
                -machine virt -kernel "${kernel_image}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpu_count}" \
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
        case_cpu_count="${qemu_cpus:-${worker_count}}"
        if ! build_and_run_case "${arch}" "${worker_count}" "${case_cpu_count}"; then
            failed=1
        fi
    done
done

if ((failed != 0)); then
    echo "[SMP] 存在失败项，请按日志定位。" >&2
    exit 1
fi
echo "[SMP] 全部架构与 worker 配置通过。"

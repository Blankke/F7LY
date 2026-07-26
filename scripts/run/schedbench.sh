#!/usr/bin/env bash
# 在决赛官方镜像的可丢弃副本中用真实 KVM 运行 schedbench。
#
# 正式验收：
#   # 分别在 riscv64 与 loongarch64 原生宿主机执行：
#   scripts/run/schedbench.sh --arch rv
#   scripts/run/schedbench.sh --arch la
# 小轮次定位：
#   scripts/run/schedbench.sh --arch rv --runs 1 --warmup 8 --rounds 64
#
# 脚本会静态交叉编译 tools/smp/schedbench.c，只向 /tmp 镜像副本注入二进制；
# 退出时删除临时镜像和二进制，并校验 images/ 下官方镜像的 SHA-256 未变化。
# 官方 final-2026 只规定 QEMU 8 vCPU/8 GiB，并未要求 KVM；本脚本按本地 SMP
# 正确性验收的更强约束禁止 TCG 回退，避免把 MTTCG 调度误当成硬件并发证据。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly SOURCE="${PROJECT_ROOT}/tools/smp/schedbench.c"
readonly LOG_ROOT="${PROJECT_ROOT}/logs/run"

arch_selection="all"
runs=3
workers=8
cpus=8
warmup=256
rounds=4096
working_set_kib=256
phase_timeout_sec=180
qemu_mem="8G"
temporary_files=()

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    local path
    for path in "${temporary_files[@]}"; do
        case "${path}" in
            /tmp/f7ly-schedbench-*)
                rm -f -- "${path}"
                ;;
            *)
                echo "拒绝清理非 schedbench 临时路径：${path}" >&2
                ;;
        esac
    done
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
用法：scripts/run/schedbench.sh [选项]

选项：
  --arch rv|la|all       架构，默认 all
  --runs N               每架构连续轮数，正式值为 3
  --workers N            worker 数，正式值为 8
  --cpus N               QEMU 与基准 CPU 数，正式值为 8
  --warmup N             预热轮数，正式值为 256
  --rounds N             测量轮数，正式值为 4096
  --working-set-kib N    每 worker 工作集，正式值为 256
  --timeout-sec N        每个阶段 watchdog，默认 180
  --qemu-mem SIZE        QEMU 内存，默认 8G
  -h, --help             显示本说明

说明：
  schedbench 禁止回退到 TCG。rv 必须在 riscv64 KVM 宿主运行，la 必须在
  loongarch64 KVM 宿主运行；双架构验收需分别在两类原生宿主执行。
EOF
}

is_positive_integer() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_integer() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

while (($# > 0)); do
    case "$1" in
        --arch) arch_selection="${2:?--arch 缺少参数}"; shift 2 ;;
        --runs) runs="${2:?--runs 缺少参数}"; shift 2 ;;
        --workers) workers="${2:?--workers 缺少参数}"; shift 2 ;;
        --cpus) cpus="${2:?--cpus 缺少参数}"; shift 2 ;;
        --warmup) warmup="${2:?--warmup 缺少参数}"; shift 2 ;;
        --rounds) rounds="${2:?--rounds 缺少参数}"; shift 2 ;;
        --working-set-kib) working_set_kib="${2:?--working-set-kib 缺少参数}"; shift 2 ;;
        --timeout-sec) phase_timeout_sec="${2:?--timeout-sec 缺少参数}"; shift 2 ;;
        --qemu-mem) qemu_mem="${2:?--qemu-mem 缺少参数}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done

[[ "${arch_selection}" =~ ^(rv|la|all)$ ]] || die "--arch 只能是 rv、la 或 all"
is_positive_integer "${runs}" || die "--runs 必须是正整数"
is_positive_integer "${workers}" || die "--workers 必须是正整数"
is_positive_integer "${cpus}" || die "--cpus 必须是正整数"
is_nonnegative_integer "${warmup}" || die "--warmup 必须是非负整数"
is_positive_integer "${rounds}" || die "--rounds 必须是正整数"
is_nonnegative_integer "${working_set_kib}" || die "--working-set-kib 必须是非负整数"
is_positive_integer "${phase_timeout_sec}" || die "--timeout-sec 必须是正整数"
((workers <= 8)) || die "内核 NCPU=8，worker 不能超过 8"
((cpus >= 2 && cpus <= 8)) || die "迁核测试要求 cpus 在 2..8"

for command_name in debugfs sha256sum timeout rg make lscpu uname; do
    command -v "${command_name}" >/dev/null || die "缺少命令：${command_name}"
done
[[ -f "${SOURCE}" ]] || die "缺少源码：${SOURCE}"
host_physical_cores="$(lscpu -p=CORE,SOCKET |
    awk -F, '!/^#/ { key=$1 FS $2; seen[key]=1 } END { print length(seen) }')"
((host_physical_cores >= cpus)) ||
    die "决赛要求至少 ${cpus} 个物理核，当前仅探测到 ${host_physical_cores} 个"

preflight_kvm_arch() {
    local arch="$1"
    local required_host_arch qemu

    case "${arch}" in
        rv)
            required_host_arch="riscv64"
            qemu="qemu-system-riscv64"
            ;;
        la)
            required_host_arch="loongarch64"
            qemu="qemu-system-loongarch64"
            ;;
        *)
            die "内部错误：未知架构 ${arch}"
            ;;
    esac

    command -v "${qemu}" >/dev/null || die "缺少 QEMU：${qemu}"
    [[ "$(uname -m)" == "${required_host_arch}" ]] ||
        die "${arch} schedbench 必须在 ${required_host_arch} 原生宿主使用 KVM；当前宿主为 $(uname -m)，禁止用 TCG 冒充"
    [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] ||
        die "${arch} schedbench 需要当前用户可读写 /dev/kvm"
    "${qemu}" -accel help 2>&1 |
        rg -q '(^|[[:space:]])kvm($|[[:space:]])' ||
        die "${qemu} 未提供 KVM accelerator"
}

if [[ "${arch_selection}" == rv || "${arch_selection}" == all ]]; then
    preflight_kvm_arch rv
fi
if [[ "${arch_selection}" == la || "${arch_selection}" == all ]]; then
    preflight_kvm_arch la
fi

run_timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="${LOG_ROOT}/schedbench-${arch_selection}-smp${cpus}-${run_timestamp}"
mkdir -p "${result_dir}"
hash_file="${result_dir}/base-images.sha256"
metrics_file="${result_dir}/metrics.tsv"
printf 'arch\trun\tstatus\tlog_file\n' >"${metrics_file}"

run_arch() {
    local arch="$1"
    local compiler kernel_arch kernel image qemu block_args
    local binary temporary_image image_hash_before image_hash_after command_line
    local run_index log_file qemu_timeout qemu_status case_count cpu_count

    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            kernel_arch="riscv"
            kernel="${PROJECT_ROOT}/kernel-rv-shell"
            image="${PROJECT_ROOT}/images/sdcard-rv-pub.img"
            qemu="qemu-system-riscv64"
            block_args=(-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0)
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            kernel_arch="loongarch"
            kernel="${PROJECT_ROOT}/kernel-la-shell"
            image="${PROJECT_ROOT}/images/sdcard-la-pub.img"
            qemu="qemu-system-loongarch64"
            block_args=(-device virtio-blk-pci,drive=x0)
            ;;
        *)
            die "内部错误：未知架构 ${arch}"
            ;;
    esac

    command -v "${compiler}" >/dev/null || die "缺少交叉编译器：${compiler}"
    command -v "${qemu}" >/dev/null || die "缺少 QEMU：${qemu}"
    [[ -f "${image}" ]] || die "缺少官方镜像：${image}"

    image_hash_before="$(sha256sum "${image}" | awk '{print $1}')"
    printf '%s\tbefore\t%s\t%s\n' "${arch}" "${image_hash_before}" "${image}" >>"${hash_file}"

    echo "[schedbench] 构建 ${arch} shell 内核与静态基准"
    make -C "${PROJECT_ROOT}" build ARCH="${kernel_arch}" INITCODE_MODE=shell

    binary="$(mktemp /tmp/f7ly-schedbench-bin-${arch}-XXXXXX)"
    temporary_files+=("${binary}")
    "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
        "${SOURCE}" -lm -o "${binary}"

    temporary_image="$(mktemp /tmp/f7ly-schedbench-image-${arch}-XXXXXX.img)"
    temporary_files+=("${temporary_image}")
    cp --reflink=auto --sparse=always "${image}" "${temporary_image}"
    debugfs -w -R 'rm /schedbench' "${temporary_image}" >/dev/null 2>&1 || true
    debugfs -w -R "write ${binary} /schedbench" "${temporary_image}" >/dev/null
    debugfs -w -R 'set_inode_field /schedbench mode 0100755' "${temporary_image}" >/dev/null

    command_line="/schedbench suite --workers ${workers} --cpus ${cpus} --warmup ${warmup} --rounds ${rounds} --migration-pattern both --working-set-kib ${working_set_kib} --timeout-sec ${phase_timeout_sec}"
    qemu_timeout=$((phase_timeout_sec * 4 + 180))

    for ((run_index = 1; run_index <= runs; ++run_index)); do
        log_file="${result_dir}/output_${arch}_run${run_index}_schedbench.txt"
        echo "[schedbench] ${arch} 第 ${run_index}/${runs} 轮：${log_file}"

        set +e
        if [[ "${arch}" == rv ]]; then
            { sleep 5; printf '%s\nexit\n' "${command_line}"; } |
                timeout "${qemu_timeout}s" "${qemu}" \
                    -machine virt -accel kvm -kernel "${kernel}" -m "${qemu_mem}" \
                    -display none -chardev stdio,id=schedbench_stdio,signal=off \
                    -serial chardev:schedbench_stdio -monitor none -smp "${cpus}" -bios default \
                    -snapshot -drive file="${temporary_image}",if=none,format=raw,id=x0 \
                    "${block_args[@]}" -no-reboot -rtc base=utc \
                    -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log_file}" 2>&1
        else
            { sleep 5; printf '%s\nexit\n' "${command_line}"; } |
                timeout "${qemu_timeout}s" "${qemu}" \
                    -machine virt -accel kvm -kernel "${kernel}" -m "${qemu_mem}" \
                    -display none -chardev stdio,id=schedbench_stdio,signal=off \
                    -serial chardev:schedbench_stdio -monitor none -smp "${cpus}" \
                    -snapshot -drive file="${temporary_image}",if=none,format=raw,id=x0 \
                    "${block_args[@]}" -no-reboot -rtc base=utc \
                    -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log_file}" 2>&1
        fi
        qemu_status=${PIPESTATUS[1]}
        set -e

        case_count="$(rg -c '^SCHEDBENCH_CASE_PASSED ' "${log_file}" || true)"
        cpu_count="$(sed -nE 's/^SCHEDBENCH_CPU .* cpu=([0-9]+) .*/\1/p' "${log_file}" |
            sort -n -u | wc -l)"
        if ((qemu_status != 0)) ||
            [[ "${case_count}" -ne 4 ]] ||
            [[ "${cpu_count}" -ne "${cpus}" ]] ||
            ! rg -q '^SCHEDBENCH_CASE_PASSED kind=wake pattern=fanout$' "${log_file}" ||
            ! rg -q '^SCHEDBENCH_CASE_PASSED kind=wake pattern=broadcast$' "${log_file}" ||
            ! rg -q '^SCHEDBENCH_CASE_PASSED kind=migrate pattern=independent$' "${log_file}" ||
            ! rg -q '^SCHEDBENCH_CASE_PASSED kind=migrate pattern=wave$' "${log_file}" ||
            ! rg -q '^SCHEDBENCH_PASSED$' "${log_file}" ||
            rg -q '(^| )(errors|target_mismatches|timeout_count|setup_errors|affinity_errors|data_errors|invalid_source_count)=[1-9][0-9]*' "${log_file}"; then
            printf '%s\t%s\tFAIL\t%s\n' "${arch}" "${run_index}" "${log_file}" >>"${metrics_file}"
            echo "[schedbench] ${arch} 第 ${run_index} 轮失败，QEMU rc=${qemu_status}" >&2
            rg -n 'SCHEDBENCH_(RESULT|CASE|FAILED|TIMEOUT)|panic:|page fault|double' "${log_file}" |
                tail -n 120 >&2 || true
            return 1
        fi

        printf '%s\t%s\tPASS\t%s\n' "${arch}" "${run_index}" "${log_file}" >>"${metrics_file}"
    done

    image_hash_after="$(sha256sum "${image}" | awk '{print $1}')"
    printf '%s\tafter\t%s\t%s\n' "${arch}" "${image_hash_after}" "${image}" >>"${hash_file}"
    [[ "${image_hash_before}" == "${image_hash_after}" ]] ||
        die "官方 ${arch} 镜像 SHA-256 发生变化"
}

failed=0
if [[ "${arch_selection}" == rv || "${arch_selection}" == all ]]; then
    run_arch rv || failed=1
fi
if [[ "${arch_selection}" == la || "${arch_selection}" == all ]]; then
    run_arch la || failed=1
fi

((failed == 0)) || exit 1
echo "[schedbench] 全部轮次通过：${metrics_file}"

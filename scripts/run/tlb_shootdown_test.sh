#!/usr/bin/env bash
# 编译并在 RV64/LA64 上运行跨 CPU TLB shootdown 专项，同时检查 8 GiB PMM 容量。
#
# 用法示例：
#   scripts/run/tlb_shootdown_test.sh
#   scripts/run/tlb_shootdown_test.sh --arch rv --rounds 20 --qemu-mem 8G
#   scripts/run/tlb_shootdown_test.sh --arch all --rounds 200 --min-managed-pages 1500000

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly SOURCE="${PROJECT_ROOT}/tools/smp/f7ly_tlb_shootdown_test.c"
readonly BUILD_DIR="${PROJECT_ROOT}/build/tlb-shootdown-test"
readonly LOG_DIR="${PROJECT_ROOT}/logs/run"

arch_selection="all"
rounds=200
qemu_mem="8G"
qemu_cpus=8
min_managed_pages=1500000
temporary_images=()

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    local image
    for image in "${temporary_images[@]}"; do
        if [[ "${image}" == /tmp/f7ly-tlb-shootdown-* ]]; then
            rm -f -- "${image}"
        fi
    done
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
用法：scripts/run/tlb_shootdown_test.sh [选项]

选项：
  --arch rv|la|all          架构，默认 all
  --rounds N                mprotect 降权/恢复轮数，默认 200
  --qemu-mem SIZE           QEMU 内存，默认 8G
  --qemu-cpus N             QEMU vCPU 数，默认 8
  --min-managed-pages N     PMM 最少管理页数，8G 默认 1500000
  -h, --help                显示本说明
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch) arch_selection="${2:?--arch 缺少参数}"; shift 2 ;;
        --rounds) rounds="${2:?--rounds 缺少参数}"; shift 2 ;;
        --qemu-mem) qemu_mem="${2:?--qemu-mem 缺少参数}"; shift 2 ;;
        --qemu-cpus) qemu_cpus="${2:?--qemu-cpus 缺少参数}"; shift 2 ;;
        --min-managed-pages) min_managed_pages="${2:?--min-managed-pages 缺少参数}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done

[[ "${arch_selection}" =~ ^(rv|la|all)$ ]] || die "--arch 只能是 rv、la 或 all"
[[ "${rounds}" =~ ^[1-9][0-9]*$ ]] || die "--rounds 必须是正整数"
[[ "${qemu_cpus}" =~ ^[2-8]$ ]] || die "--qemu-cpus 必须在 2..8"
[[ "${min_managed_pages}" =~ ^[0-9]+$ ]] || die "--min-managed-pages 必须是非负整数"
command -v debugfs >/dev/null || die "缺少 debugfs"
command -v e2fsck >/dev/null || die "缺少 e2fsck"
mkdir -p "${BUILD_DIR}" "${LOG_DIR}"

check_and_repair_image() {
    local image="$1" rc
    set +e
    e2fsck -pf "${image}" >/dev/null 2>&1
    rc=$?
    set -e
    ((rc <= 1)) || die "临时镜像 e2fsck 失败：${image}，退出码=${rc}"
}

run_arch() {
    local arch="$1"
    local compiler kernel_profile kernel rootfs qemu binary temporary_rootfs timestamp log timeout_seconds rc managed_pages
    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            kernel_profile="riscv-qemu"
            kernel="${PROJECT_ROOT}/kernel-rv-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-rv.img"
            qemu="qemu-system-riscv64"
            binary="${BUILD_DIR}/f7ly_tlb_shootdown_test-rv"
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            kernel_profile="loongarch-qemu"
            kernel="${PROJECT_ROOT}/kernel-la-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-la.img"
            qemu="qemu-system-loongarch64"
            binary="${BUILD_DIR}/f7ly_tlb_shootdown_test-la"
            ;;
    esac

    command -v "${compiler}" >/dev/null || die "缺少交叉编译器：${compiler}"
    command -v "${qemu}" >/dev/null || die "缺少 QEMU：${qemu}"
    [[ -f "${rootfs}" ]] || die "缺少 rootfs：${rootfs}"

    echo "[TLB] 构建 ${arch} shell 内核与静态专项"
    make -C "${PROJECT_ROOT}" build PROFILE="${kernel_profile}" MODE=shell
    "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
        "${SOURCE}" -o "${binary}"

    temporary_rootfs="$(mktemp /tmp/f7ly-tlb-shootdown-${arch}-XXXXXX.img)"
    temporary_images+=("${temporary_rootfs}")
    cp --reflink=auto "${rootfs}" "${temporary_rootfs}"
    check_and_repair_image "${temporary_rootfs}"
    debugfs -w -R 'rm /f7ly_tlb_shootdown_test' "${temporary_rootfs}" >/dev/null 2>&1 || true
    debugfs -w -R "write ${binary} /f7ly_tlb_shootdown_test" "${temporary_rootfs}" >/dev/null
    check_and_repair_image "${temporary_rootfs}"

    timestamp="$(date +%Y%m%d-%H%M%S)"
    log="${LOG_DIR}/output_${arch}_smp${qemu_cpus}_mem${qemu_mem}_tlb_shootdown_${timestamp}.txt"
    timeout_seconds=$((rounds / 2 + 300))
    echo "[TLB] 运行 ${arch}，日志：${log}"

    set +e
    if [[ "${arch}" == rv ]]; then
        { sleep 5; printf '/f7ly_tlb_shootdown_test --rounds %d\nexit\n' "${rounds}"; } |
            timeout "${timeout_seconds}s" "${qemu}" \
                -machine virt -kernel "${kernel}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpus}" -bios default \
                -drive file="${temporary_rootfs}",if=none,format=raw,id=x0 \
                -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
                -device virtio-net-device,netdev=net -netdev user,id=net \
                -no-reboot -rtc base=utc >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    else
        { sleep 5; printf '/f7ly_tlb_shootdown_test --rounds %d\nexit\n' "${rounds}"; } |
            timeout "${timeout_seconds}s" "${qemu}" \
                -machine virt -kernel "${kernel}" -m "${qemu_mem}" \
                -display none -chardev stdio,id=shell_stdio,signal=off \
                -serial chardev:shell_stdio -monitor none -smp "${qemu_cpus}" \
                -drive file="${temporary_rootfs}",if=none,format=raw,id=x0 \
                -device virtio-blk-pci,drive=x0 \
                -netdev user,id=net -device virtio-net-pci,netdev=net \
                -no-reboot -rtc base=utc >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    fi
    set -e

    managed_pages="$(sed -nE 's/^F7LY_TLB_SHOOTDOWN_MEMORY .*managed_pages=([0-9]+).*$/\1/p' "${log}" | tail -n 1)"
    if ((rc != 0)) || ! rg -q '^F7LY_TLB_SHOOTDOWN_PASS ' "${log}"; then
        echo "[TLB] ${arch} 专项失败，QEMU rc=${rc}" >&2
        rg -n 'F7LY_TLB|\[tlb\]|\[pmm\]|panic|unexpected scause|page fault' "${log}" | tail -n 180 || true
        return 1
    fi
    if [[ -z "${managed_pages}" || "${managed_pages}" -lt "${min_managed_pages}" ]]; then
        echo "[TLB] ${arch} PMM 容量不足：${managed_pages:-unknown} < ${min_managed_pages}" >&2
        return 1
    fi
    echo "[TLB] PASS ${arch}：managed_pages=${managed_pages}，${log}"
}

failed=0
if [[ "${arch_selection}" == rv || "${arch_selection}" == all ]]; then
    run_arch rv || failed=1
fi
if [[ "${arch_selection}" == la || "${arch_selection}" == all ]]; then
    run_arch la || failed=1
fi
((failed == 0)) || exit 1
echo "[TLB] 双架构 shootdown 与动态内存检查全部通过。"

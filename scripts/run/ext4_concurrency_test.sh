#!/usr/bin/env bash
#
# 在临时 shell 镜像中运行 ext4 并发文件操作专项，并对写回后的临时镜像做只读 fsck。
#
# 用法示例：
#   scripts/run/ext4_concurrency_test.sh --arch all
#   scripts/run/ext4_concurrency_test.sh --arch rv --qemu-cpus 8 --qemu-mem 8G
#
# 该脚本会编译 shell 内核和静态测试程序，但不会运行 BuildStorm 或 Cargo 自编译。
# 原始 images/sdcard-*.img 始终只读，所有 guest 写入都落在 /tmp 下的副本。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly SOURCE="${PROJECT_ROOT}/tools/fs/f7ly_ext4_concurrency_test.c"
readonly BUILD_DIR="${PROJECT_ROOT}/build/ext4-concurrency-test"
readonly LOG_DIR="${PROJECT_ROOT}/logs/run"

arch_selection="all"
qemu_mem="8G"
qemu_cpus=8
temporary_images=()

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    local image
    for image in "${temporary_images[@]}"; do
        if [[ "${image}" == /tmp/f7ly-ext4-* ]]; then
            rm -f -- "${image}"
        fi
    done
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
用法：scripts/run/ext4_concurrency_test.sh [选项]

选项：
  --arch rv|la|all       架构，默认 all
  --qemu-cpus N          vCPU 数量，默认 8
  --qemu-mem SIZE        QEMU 内存，默认 8G
  -h, --help             显示本说明
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch) arch_selection="${2:?--arch 缺少参数}"; shift 2 ;;
        --qemu-cpus) qemu_cpus="${2:?--qemu-cpus 缺少参数}"; shift 2 ;;
        --qemu-mem) qemu_mem="${2:?--qemu-mem 缺少参数}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done

[[ "${arch_selection}" =~ ^(rv|la|all)$ ]] || die "--arch 只能是 rv、la 或 all"
[[ "${qemu_cpus}" =~ ^[1-8]$ ]] || die "--qemu-cpus 必须在 1..8"
command -v debugfs >/dev/null || die "缺少 debugfs"
command -v e2fsck >/dev/null || die "缺少 e2fsck"
command -v timeout >/dev/null || die "缺少 timeout"
mkdir -p "${BUILD_DIR}" "${LOG_DIR}"

check_fsck() {
    local mode="$1" image="$2" rc
    set +e
    e2fsck "${mode}" "${image}" >/dev/null 2>&1
    rc=$?
    set -e
    # e2fsck -pf 在自动修复后允许返回 1；只读 -fn 必须严格返回 0。
    if [[ "${mode}" == "-fn" ]]; then
        ((rc == 0)) || die "临时镜像只读 fsck 失败：${image}，退出码=${rc}"
    else
        ((rc <= 1)) || die "临时镜像 fsck 失败：${image}，退出码=${rc}"
    fi
}

run_arch() {
    local arch="$1"
    local compiler make_arch kernel rootfs qemu binary temp_image timestamp log rc
    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            make_arch="riscv"
            kernel="${PROJECT_ROOT}/kernel-rv-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-rv.img"
            qemu="qemu-system-riscv64"
            binary="${BUILD_DIR}/f7ly_ext4_concurrency_test-rv"
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            make_arch="loongarch"
            kernel="${PROJECT_ROOT}/kernel-la-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-la.img"
            qemu="qemu-system-loongarch64"
            binary="${BUILD_DIR}/f7ly_ext4_concurrency_test-la"
            ;;
    esac

    command -v "${compiler}" >/dev/null || die "缺少交叉编译器：${compiler}"
    command -v "${qemu}" >/dev/null || die "缺少 QEMU：${qemu}"
    [[ -f "${rootfs}" ]] || die "缺少 shell 存储镜像：${rootfs}"

    echo "[EXT4] 构建 ${arch} shell 内核与静态专项"
    make -C "${PROJECT_ROOT}" build "ARCH=${make_arch}" INITCODE_MODE=shell \
        >"${LOG_DIR}/build-${arch}-ext4-concurrency-$(date +%Y%m%d-%H%M%S).log" 2>&1
    "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
        "${SOURCE}" -o "${binary}"

    temp_image="$(mktemp /tmp/f7ly-ext4-${arch}-XXXXXX.img)"
    temporary_images+=("${temp_image}")
    cp --reflink=auto "${rootfs}" "${temp_image}"
    check_fsck -pf "${temp_image}"
    debugfs -w -R 'rm /f7ly_ext4_concurrency_test' "${temp_image}" >/dev/null 2>&1 || true
    debugfs -w -R "write ${binary} /f7ly_ext4_concurrency_test" "${temp_image}" >/dev/null
    debugfs -w -R 'set_inode_field /f7ly_ext4_concurrency_test mode 0100755' \
        "${temp_image}" >/dev/null
    check_fsck -pf "${temp_image}"

    timestamp="$(date +%Y%m%d-%H%M%S)"
    log="${LOG_DIR}/output_${arch}_ext4_concurrency_smp${qemu_cpus}_mem${qemu_mem}_${timestamp}.txt"
    echo "[EXT4] 运行 ${arch}，日志：${log}"
    set +e
    if [[ "${arch}" == rv ]]; then
        { sleep 5; printf '/f7ly_ext4_concurrency_test\nexit\n'; } |
            timeout 180s "${qemu}" -machine virt -kernel "${kernel}" -m "${qemu_mem}" \
                -accel tcg,thread=multi -display none \
                -chardev stdio,id=shell_stdio,signal=off -serial chardev:shell_stdio \
                -monitor none -smp "${qemu_cpus}" -bios default \
                -drive file="${temp_image}",if=none,format=raw,id=x0 \
                -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
                -device virtio-net-device,netdev=net -netdev user,id=net \
                -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" \
                >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    else
        { sleep 5; printf '/f7ly_ext4_concurrency_test\nexit\n'; } |
            timeout 180s "${qemu}" -machine virt -kernel "${kernel}" -m "${qemu_mem}" \
                -accel tcg,thread=multi -display none \
                -chardev stdio,id=shell_stdio,signal=off -serial chardev:shell_stdio \
                -monitor none -smp "${qemu_cpus}" \
                -drive file="${temp_image}",if=none,format=raw,id=x0 \
                -device virtio-blk-pci,drive=x0 -device virtio-net-pci,netdev=net \
                -netdev user,id=net -no-reboot -rtc base=utc \
                -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    fi
    set -e

    check_fsck -fn "${temp_image}"
    if ((rc != 0)) || ! rg -q '^F7LY_EXT4_CONCURRENCY_PASS ' "${log}"; then
        echo "[EXT4] ${arch} 专项失败，QEMU rc=${rc}" >&2
        rg -n 'F7LY_EXT4|panic|unexpected scause|page fault|lock|timeout' "${log}" | tail -n 120 || true
        return 1
    fi
    echo "[EXT4] PASS ${arch}：${log}"
}

if pgrep -f 'qemu-system-(riscv64|loongarch64)' >/dev/null 2>&1; then
    die "检测到其它 QEMU 正在运行，拒绝并行启动专项"
fi

failed=0
if [[ "${arch_selection}" == rv || "${arch_selection}" == all ]]; then
    run_arch rv || failed=1
fi
if [[ "${arch_selection}" == la || "${arch_selection}" == all ]]; then
    run_arch la || failed=1
fi
((failed == 0)) || exit 1
echo "[EXT4] 双架构并发文件操作与临时镜像只读 fsck 全部通过。"

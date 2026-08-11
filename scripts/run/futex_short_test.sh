#!/usr/bin/env bash
#
# 在临时 shell 镜像中运行 F7LY futex/线程退出短测。
#
# 使用示例：
#   scripts/run/futex_short_test.sh --arch all
#   scripts/run/futex_short_test.sh --arch rv --qemu-cpus 8 --timeout 120
#
# 每个架构只运行一个 QEMU；原始 images/sdcard-*.img 不会被写入，测试程序
# 和 guest 产生的文件都落在 /tmp 的镜像副本中。本脚本不启动 BuildStorm/Cargo。

set -euo pipefail

readonly PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly SOURCE="${PROJECT_ROOT}/tools/proc/f7ly_futex_short_test.c"
readonly BUILD_DIR="${PROJECT_ROOT}/build/futex-short-test"
readonly LOG_DIR="${PROJECT_ROOT}/logs/run"

arch_selection="all"
qemu_cpus=8
qemu_timeout=120
temporary_images=()

die() {
    echo "错误：$*" >&2
    exit 2
}

cleanup() {
    local image
    for image in "${temporary_images[@]}"; do
        if [[ "${image}" == /tmp/f7ly-futex-* ]]; then
            rm -f -- "${image}"
        fi
    done
}
trap cleanup EXIT

while (($# > 0)); do
    case "$1" in
        --arch) arch_selection="${2:?--arch 缺少参数}"; shift 2 ;;
        --qemu-cpus) qemu_cpus="${2:?--qemu-cpus 缺少参数}"; shift 2 ;;
        --timeout) qemu_timeout="${2:?--timeout 缺少参数}"; shift 2 ;;
        -h|--help)
            sed -n '1,14p' "$0"
            exit 0
            ;;
        *) die "未知参数：$1" ;;
    esac
done

[[ "${arch_selection}" =~ ^(rv|la|all)$ ]] || die "--arch 只能是 rv、la 或 all"
[[ "${qemu_cpus}" =~ ^[1-8]$ ]] || die "--qemu-cpus 必须在 1..8"
[[ "${qemu_timeout}" =~ ^[1-9][0-9]*$ ]] || die "--timeout 必须是正整数秒"
command -v debugfs >/dev/null || die "缺少 debugfs"
command -v timeout >/dev/null || die "缺少 timeout"
mkdir -p "${BUILD_DIR}" "${LOG_DIR}"

run_arch() {
    local arch="$1"
    local compiler make_profile kernel rootfs qemu binary temp_image timestamp log rc
    case "${arch}" in
        rv)
            compiler="riscv64-linux-gnu-gcc"
            make_profile="riscv-qemu"
            kernel="${PROJECT_ROOT}/kernel-rv-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-rv.img"
            qemu="qemu-system-riscv64"
            binary="${BUILD_DIR}/f7ly_futex_short_test-rv"
            ;;
        la)
            compiler="loongarch64-linux-gnu-gcc"
            make_profile="loongarch-qemu"
            kernel="${PROJECT_ROOT}/kernel-la-shell"
            rootfs="${PROJECT_ROOT}/images/sdcard-la.img"
            qemu="qemu-system-loongarch64"
            binary="${BUILD_DIR}/f7ly_futex_short_test-la"
            ;;
    esac

    command -v "${compiler}" >/dev/null || die "缺少交叉编译器：${compiler}"
    command -v "${qemu}" >/dev/null || die "缺少 QEMU：${qemu}"
    [[ -f "${rootfs}" ]] || die "缺少 shell 镜像：${rootfs}"

    echo "[FUTEX] 构建 ${arch} shell 内核与静态短测"
    make -C "${PROJECT_ROOT}" build PROFILE="${make_profile}" MODE=shell \
        >"${LOG_DIR}/build-${arch}-futex-short-$(date +%Y%m%d-%H%M%S).log" 2>&1
    "${compiler}" -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
        "${SOURCE}" -o "${binary}"

    temp_image="$(mktemp /tmp/f7ly-futex-${arch}-XXXXXX.img)"
    temporary_images+=("${temp_image}")
    cp --reflink=auto "${rootfs}" "${temp_image}"
    debugfs -w -R 'rm /f7ly_futex_short_test' "${temp_image}" >/dev/null 2>&1 || true
    debugfs -w -R "write ${binary} /f7ly_futex_short_test" "${temp_image}" >/dev/null
    debugfs -w -R 'set_inode_field /f7ly_futex_short_test mode 0100755' \
        "${temp_image}" >/dev/null

    timestamp="$(date +%Y%m%d-%H%M%S)"
    log="${LOG_DIR}/output_${arch}_futex_short_smp${qemu_cpus}_${timestamp}.txt"
    echo "[FUTEX] 运行 ${arch}，日志：${log}"
    set +e
    if [[ "${arch}" == rv ]]; then
        { sleep 5; printf '/f7ly_futex_short_test\nexit\n'; } |
            timeout "${qemu_timeout}s" "${qemu}" -machine virt -kernel "${kernel}" \
                -m 8G -accel tcg,thread=multi -display none \
                -chardev stdio,id=shell_stdio,signal=off -serial chardev:shell_stdio \
                -monitor none -smp "${qemu_cpus}" -bios default \
                -drive file="${temp_image}",if=none,format=raw,id=x0 \
                -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
                -device virtio-net-device,netdev=net -netdev user,id=net \
                -no-reboot -rtc base=utc -initrd "${PROJECT_ROOT}/images/initrd.img" \
                >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    else
        { sleep 5; printf '/f7ly_futex_short_test\nexit\n'; } |
            timeout "${qemu_timeout}s" "${qemu}" -machine virt -kernel "${kernel}" \
                -m 8G -accel tcg,thread=multi -display none \
                -chardev stdio,id=shell_stdio,signal=off -serial chardev:shell_stdio \
                -monitor none -smp "${qemu_cpus}" \
                -drive file="${temp_image}",if=none,format=raw,id=x0 \
                -device virtio-blk-pci,drive=x0 -device virtio-net-pci,netdev=net \
                -netdev user,id=net -no-reboot -rtc base=utc \
                -initrd "${PROJECT_ROOT}/images/initrd.img" >"${log}" 2>&1
        rc=${PIPESTATUS[1]}
    fi
    set -e

    if ((rc != 0)) || ! rg -q '^F7LY_FUTEX_SHORT_PASS ' "${log}"; then
        echo "[FUTEX] ${arch} 专项失败，QEMU rc=${rc}" >&2
        rg -n 'F7LY_FUTEX|panic|unexpected scause|page fault|timeout|lock' "${log}" | tail -n 120 || true
        return 1
    fi
    echo "[FUTEX] PASS ${arch}：${log}"
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
echo "[FUTEX] 双架构 futex/线程退出短测全部通过。"

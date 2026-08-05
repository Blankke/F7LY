#!/usr/bin/env bash

set -Eeuo pipefail

ROOTFS="${1:-/mnt/sdcard-rv-pub}"
TEST_SCRIPT="${2:-/glibc/buildstorm_testcode.sh}"

# 默认只给测试看到 4 个 CPU。
# 如果想使用宿主全部 CPU：
#   CPUSET=all ./run_buildstorm_qemu.sh
#
# 如果想模拟 8 核：
#   CPUSET=0-7 ./run_buildstorm_qemu.sh
CPUSET="${CPUSET:-0-7}"

# ------------------------------------------------------------
# 提升到 root
# ------------------------------------------------------------

if [ "$(id -u)" -ne 0 ]; then
    exec sudo --preserve-env=CPUSET \
        "$(readlink -f "$0")" "$@"
fi

# ------------------------------------------------------------
# 基本检查
# ------------------------------------------------------------

if [ ! -d "$ROOTFS" ]; then
    echo "ERROR: rootfs does not exist: $ROOTFS" >&2
    exit 1
fi

if [ ! -f "$ROOTFS$TEST_SCRIPT" ]; then
    echo "ERROR: test script does not exist:" >&2
    echo "       $ROOTFS$TEST_SCRIPT" >&2
    exit 1
fi

if [ ! -d "$ROOTFS/work/tgoskits" ]; then
    echo "ERROR: /work/tgoskits not found in rootfs" >&2
    exit 1
fi

QEMU="$(command -v qemu-riscv64-static || true)"

if [ -z "$QEMU" ]; then
    echo "ERROR: qemu-riscv64-static not found" >&2
    echo
    echo "Install it with:"
    echo "    sudo apt install qemu-user-static binfmt-support"
    exit 1
fi

echo "ROOTFS      = $ROOTFS"
echo "TEST_SCRIPT = $TEST_SCRIPT"
echo "QEMU        = $QEMU"
echo "CPUSET      = $CPUSET"
echo

# ------------------------------------------------------------
# 准备 binfmt_misc
#
# 这里非常重要：
# 第一个 /bin/sh 可以手动由 qemu 启动，
# 但随后 shell exec rustc/cargo/find/awk/... 时，
# 仍然需要 binfmt_misc 自动调用 qemu。
# ------------------------------------------------------------

BINFMT_DIR="/proc/sys/fs/binfmt_misc"
BINFMT_ENTRY="$BINFMT_DIR/qemu-riscv64"

BINFMT_MOUNTED_BY_US=0
BINFMT_ENABLED_BY_US=0
BINFMT_REENABLED_BY_US=0

cleanup_binfmt()
{
    set +e

    if [ "$BINFMT_REENABLED_BY_US" -eq 1 ] && [ -e "$BINFMT_ENTRY" ]; then
        echo 0 > "$BINFMT_ENTRY" 2>/dev/null || true
    fi

    if [ "$BINFMT_ENABLED_BY_US" -eq 1 ]; then
        if command -v update-binfmts >/dev/null 2>&1; then
            update-binfmts --disable qemu-riscv64 >/dev/null 2>&1 || true
        fi
    fi

    if [ "$BINFMT_MOUNTED_BY_US" -eq 1 ]; then
        umount "$BINFMT_DIR" >/dev/null 2>&1 || true
    fi
}

trap cleanup_binfmt EXIT

mkdir -p "$BINFMT_DIR"

if ! mountpoint -q "$BINFMT_DIR"; then
    echo "[host] mounting binfmt_misc"
    mount -t binfmt_misc binfmt_misc "$BINFMT_DIR"
    BINFMT_MOUNTED_BY_US=1
fi

# 已经存在但 disabled
if [ -e "$BINFMT_ENTRY" ]; then
    if grep -q '^disabled' "$BINFMT_ENTRY" 2>/dev/null; then
        echo "[host] temporarily enabling existing qemu-riscv64 binfmt"
        echo 1 > "$BINFMT_ENTRY"
        BINFMT_REENABLED_BY_US=1
    fi
else
    # 尝试 Debian/Ubuntu 的 update-binfmts
    if command -v update-binfmts >/dev/null 2>&1; then
        echo "[host] temporarily enabling qemu-riscv64 binfmt"
        update-binfmts --enable qemu-riscv64 || true

        if [ -e "$BINFMT_ENTRY" ]; then
            BINFMT_ENABLED_BY_US=1
        fi
    fi
fi

if [ ! -e "$BINFMT_ENTRY" ]; then
    echo
    echo "ERROR: qemu-riscv64 binfmt is not registered."
    echo
    echo "Run once:"
    echo
    echo "    sudo apt install qemu-user-static binfmt-support"
    echo "    sudo update-binfmts --enable qemu-riscv64"
    echo
    exit 1
fi

echo
echo "---------- binfmt ----------"
cat "$BINFMT_ENTRY"
echo "----------------------------"
echo

# ------------------------------------------------------------
# 取出 binfmt interpreter 信息
# ------------------------------------------------------------

BINFMT_INTERPRETER="$(
    awk '$1 == "interpreter" { print $2 }' "$BINFMT_ENTRY" 2>/dev/null || true
)"

BINFMT_FLAGS="$(
    awk '$1 == "flags:" { print $2 }' "$BINFMT_ENTRY" 2>/dev/null || true
)"

echo "binfmt interpreter = ${BINFMT_INTERPRETER:-unknown}"
echo "binfmt flags       = ${BINFMT_FLAGS:-unknown}"
echo

# ------------------------------------------------------------
# 新 mount namespace
#
# 之后的 overlay/proc/sys/dev mount 全部只存在于这个 namespace。
# 即使 Ctrl-C，namespace 消失后这些 mount 也不会留在宿主。
# ------------------------------------------------------------

unshare --mount --fork bash -s -- \
    "$ROOTFS" \
    "$TEST_SCRIPT" \
    "$QEMU" \
    "$CPUSET" \
    "$BINFMT_INTERPRETER" \
    "$BINFMT_FLAGS" <<'INNER_SCRIPT'

set -Eeuo pipefail

ROOTFS="$1"
TEST_SCRIPT="$2"
QEMU="$3"
CPUSET="$4"
BINFMT_INTERPRETER="$5"
BINFMT_FLAGS="$6"

# 防止 mount 传播回宿主 namespace
mount --make-rprivate /

TMPDIR_RUN="$(mktemp -d /var/tmp/buildstorm-qemu.XXXXXX)"

UPPER="$TMPDIR_RUN/upper"
WORK="$TMPDIR_RUN/work"
MERGED="$TMPDIR_RUN/root"

mkdir -p "$UPPER" "$WORK" "$MERGED"

cleanup()
{
    set +e

    echo
    echo "[cleanup] removing temporary QEMU rootfs"

    # -R 会递归卸载 merged 下的 proc/sys/dev/qemu bind mount
    if mountpoint -q "$MERGED"; then
        umount -R "$MERGED" 2>/dev/null || umount -l "$MERGED" 2>/dev/null || true
    fi

    rm -rf "$TMPDIR_RUN"
}

trap cleanup EXIT INT TERM HUP

# ------------------------------------------------------------
# OverlayFS:
#
# lower = 原始 SD rootfs
# upper = 临时写层
#
# 所以 BuildStorm 无论怎么 rm/cargo build，都不会改原卡。
# ------------------------------------------------------------

echo "[setup] creating temporary OverlayFS"

mount -t overlay overlay \
    -o "lowerdir=$ROOTFS,upperdir=$UPPER,workdir=$WORK" \
    "$MERGED"

# ------------------------------------------------------------
# 准备 qemu-riscv64-static
#
# chroot 以后也必须能够找到这个文件。
# 这里的 touch 只写 Overlay upper，不改 SD 卡。
# ------------------------------------------------------------

mkdir -p "$MERGED/usr/bin"

if [ ! -e "$MERGED/usr/bin/qemu-riscv64-static" ]; then
    touch "$MERGED/usr/bin/qemu-riscv64-static"
fi

mount --bind \
    "$QEMU" \
    "$MERGED/usr/bin/qemu-riscv64-static"

mount -o remount,bind,ro \
    "$MERGED/usr/bin/qemu-riscv64-static"

# ------------------------------------------------------------
# 如果 binfmt 没有 F flag，kernel 会在 chroot 内按 interpreter
# 路径查 QEMU，因此额外把 static QEMU bind 到对应位置。
# ------------------------------------------------------------

if [[ "$BINFMT_FLAGS" != *F* ]] &&
   [ -n "$BINFMT_INTERPRETER" ] &&
   [ "$BINFMT_INTERPRETER" != "/usr/bin/qemu-riscv64-static" ]; then

    echo "[setup] binfmt has no F flag; exposing interpreter inside chroot:"
    echo "        $BINFMT_INTERPRETER"

    mkdir -p "$MERGED$(dirname "$BINFMT_INTERPRETER")"

    if [ ! -e "$MERGED$BINFMT_INTERPRETER" ]; then
        touch "$MERGED$BINFMT_INTERPRETER"
    fi

    mount --bind "$QEMU" "$MERGED$BINFMT_INTERPRETER"
    mount -o remount,bind,ro "$MERGED$BINFMT_INTERPRETER"
fi

# ------------------------------------------------------------
# 给 guest rootfs 提供 Linux 虚拟文件系统
# ------------------------------------------------------------

echo "[setup] mounting proc/sys/dev"

mount -t proc proc "$MERGED/proc"

mount --rbind /sys "$MERGED/sys"
mount --make-rslave "$MERGED/sys"

mount --rbind /dev "$MERGED/dev"
mount --make-rslave "$MERGED/dev"

echo
echo "============================================================"
echo " BuildStorm under qemu-riscv64 user-mode"
echo "============================================================"
echo

# ------------------------------------------------------------
# 正式运行
#
# 第一个 /bin/sh 显式交给 qemu。
# shell 后续 exec 的 RISC-V ELF 则由 binfmt_misc 接管。
# ------------------------------------------------------------

set +e

if [ "$CPUSET" = "all" ]; then

    chroot "$MERGED" \
        /usr/bin/qemu-riscv64-static \
        /bin/sh \
        "$TEST_SCRIPT"

    RC=$?

else

    taskset -c "$CPUSET" \
        chroot "$MERGED" \
        /usr/bin/qemu-riscv64-static \
        /bin/sh \
        "$TEST_SCRIPT"

    RC=$?

fi

set -e

echo
echo "============================================================"
echo " BuildStorm exited with rc=$RC"
echo "============================================================"

exit "$RC"

INNER_SCRIPT
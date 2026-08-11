#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DIR="$REPO_ROOT/images"

# 挂载点可能不存在，先统一创建目录，避免 mount 因目标路径缺失失败。
sudo mkdir -p \
    /mnt/sdcard-rv \
    /mnt/sdcard-la \
    /mnt/rootfs-rv \
    /mnt/rootfs-la

sudo mount -o loop "$IMAGE_DIR/oscomp-preliminary-riscv64.img" /mnt/sdcard-rv
sudo mount -o loop "$IMAGE_DIR/oscomp-preliminary-loongarch64.img" /mnt/sdcard-la
sudo mount -o loop "$IMAGE_DIR/oscomp-final-riscv64.img" /mnt/rootfs-rv
sudo mount -o loop "$IMAGE_DIR/oscomp-final-loongarch64.img" /mnt/rootfs-la
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1

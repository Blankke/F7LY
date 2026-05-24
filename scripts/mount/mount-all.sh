#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DIR="$REPO_ROOT/images"

sudo mount -o loop "$IMAGE_DIR/sdcard-rv.img" /mnt/sdcard-rv
sudo mount -o loop "$IMAGE_DIR/sdcard-la.img" /mnt/sdcard-la
sudo mount -o loop "$IMAGE_DIR/sdcard-rv-final.img" /mnt/sdcard-rv-final
sudo mount -o loop "$IMAGE_DIR/sdcard-la-final.img" /mnt/sdcard-la-final
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1
sudo mount -o loop "$IMAGE_DIR/sdcard-rv-onsite.img" /mnt/sdcard-rv-onsite
sudo mount -o loop "$IMAGE_DIR/sdcard-la-onsite.img" /mnt/sdcard-la-onsite

#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

sudo umount /mnt/sdcard-rv
sudo mount -o loop "$REPO_ROOT/images/sdcard-rv-onsite.img" /mnt/sdcard-rv
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1

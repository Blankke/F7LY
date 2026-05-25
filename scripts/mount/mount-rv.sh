#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MOUNT_DIR="/mnt/sdcard-rv"

# 挂载点可能不存在，先创建目录，避免 mount 因目标路径缺失失败。
sudo mkdir -p "$MOUNT_DIR"
sudo mount -o loop "$REPO_ROOT/images/sdcard-rv.img" "$MOUNT_DIR"
sudo ln -sf "$MOUNT_DIR/musl/lib/libc.so" /lib/ld-musl-riscv64.so.1

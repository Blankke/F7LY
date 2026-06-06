#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MOUNT_DIR="/mnt/rootfs-la"

# 挂载点可能不存在，先创建目录，避免 mount 因目标路径缺失失败。
sudo mkdir -p "$MOUNT_DIR"
sudo mount -o loop "$REPO_ROOT/images/rootfs-loongarch64.img" "$MOUNT_DIR"
cd "$MOUNT_DIR"

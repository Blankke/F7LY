#!/bin/sh
set -eu

# 从备份镜像恢复当前运行镜像。cp 会覆盖目标文件，避免额外 rm 降低误删风险。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DIR="$REPO_ROOT/images"

cp "$IMAGE_DIR/sdcard-bak/sdcard-la.img" "$IMAGE_DIR/sdcard-la.img"
cp "$IMAGE_DIR/sdcard-bak/sdcard-rv.img" "$IMAGE_DIR/sdcard-rv.img"

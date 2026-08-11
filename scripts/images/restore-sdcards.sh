#!/bin/sh
set -eu

# 从 bak 基线覆盖恢复四份 QEMU 工作镜像。这是显式恢复工具；日常
# make run/shell 只会在工作镜像缺失时自动恢复，不会覆盖已有修改。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DIR="$REPO_ROOT/images"
BACKUP_DIR="$IMAGE_DIR/bak"

for image_name in \
    oscomp-preliminary-riscv64.img \
    oscomp-preliminary-loongarch64.img \
    oscomp-final-riscv64.img \
    oscomp-final-loongarch64.img
do
    backup="$BACKUP_DIR/$image_name"
    target="$IMAGE_DIR/$image_name"
    if [ ! -s "$backup" ]; then
        echo "错误：bak 基线不存在或为空: $backup" >&2
        echo "请先运行对应的 make prepare-image PROFILE=... MODE=..." >&2
        exit 1
    fi

    temporary="$target.tmp.$$"
    trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
    cp --reflink=auto --sparse=always -- "$backup" "$temporary"
    mv -- "$temporary" "$target"
    trap - EXIT HUP INT TERM
    echo "[image] 已恢复: $target"
done

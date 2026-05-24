#!/bin/sh
set -eu

# 允许从任意目录执行脚本，避免依赖当前工作目录。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

sudo mount -o loop "$REPO_ROOT/images/sdcard-la.img" /mnt/sdcard-la
cd /mnt/sdcard-la

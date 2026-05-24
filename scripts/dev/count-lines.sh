#!/bin/bash
set -euo pipefail

# 允许从任意目录统计源码行数，避免脚本路径调整后依赖调用位置。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

find "$REPO_ROOT/kernel" "$REPO_ROOT/user" \
    \( -name '*.cc' -o -name '*.c' -o -name '*.S' -o -name '*.hh' -o -name '*.h' -o -name '*.inc' -o -name 'Makefile' \) \
    -print0 | xargs -0 wc -l

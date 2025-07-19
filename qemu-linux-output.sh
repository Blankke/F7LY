#!/bin/bash

# 检查是否提供了程序名参数
if [ $# -eq 0 ]; then
    echo "Usage: $0 <program_name>"
    echo "Example: $0 ls"
    exit 1
fi

program_name=$1
program_path="/mnt/sdcard-rv/musl/ltp/testcases/bin/$program_name"

# 检查程序是否存在
if [ ! -f "$program_path" ]; then
    echo "Error: Program '$program_name' not found at $program_path"
    exit 1
fi

# 使用 qemu-riscv64-static 的 strace 模式执行程序
qemu-riscv64-static -strace "$program_path"
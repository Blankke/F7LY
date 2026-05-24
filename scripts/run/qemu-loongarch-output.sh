#!/bin/bash

# 检查是否提供了程序名参数
if [ $# -eq 0 ]; then
    echo "Usage: $0 <program_name>"
    echo "Example: $0 ls"
    exit 1
fi

program_name=$1
program_path="/mnt/sdcard-la/glibc/ltp/testcases/bin/$program_name"

# 检查程序是否存在
if [ ! -f "$program_path" ]; then
    echo "Error: Program '$program_name' not found at $program_path"
    exit 1
fi

# 检查和创建符号链接
link1="/usr/lib64/libc.so.6"
target1="/mnt/sdcard-la/glibc/lib/libc.so.6"
link2="/lib64/ld-linux-loongarch-lp64d.so.1"
target2="/mnt/sdcard-la/glibc/lib/ld-linux-loongarch-lp64d.so.1"
link3="/usr/lib64/libm.so.6"
target3="/mnt/sdcard-la/glibc/lib/libm.so.6"

# 确保目录存在
link1_dir=$(dirname "$link1")
if [ ! -d "$link1_dir" ]; then
    # echo "Creating directory: $link1_dir"
    sudo mkdir -p "$link1_dir"
fi

link2_dir=$(dirname "$link2")
if [ ! -d "$link2_dir" ]; then
    # echo "Creating directory: $link2_dir"
    sudo mkdir -p "$link2_dir"
fi

link3_dir=$(dirname "$link3")
if [ ! -d "$link3_dir" ]; then
    # echo "Creating directory: $link3_dir"
    sudo mkdir -p "$link3_dir"
fi

# 检查第一个符号链接
if [ -L "$link1" ]; then
    current_target1=$(readlink "$link1")
    if [ "$current_target1" = "$target1" ]; then
        # echo "Symlink $link1 already points to correct target: $target1"
        :
    else
        # echo "Symlink $link1 points to wrong target ($current_target1), removing and recreating"
        sudo rm "$link1"
        sudo ln -s "$target1" "$link1"
        # echo "Created symlink: $link1 -> $target1"
    fi
else
    # echo "Creating new symlink: $link1 -> $target1"
    sudo ln -s "$target1" "$link1"
fi

# 检查第二个符号链接
if [ -L "$link2" ]; then
    current_target2=$(readlink "$link2")
    if [ "$current_target2" = "$target2" ]; then
        # echo "Symlink $link2 already points to correct target: $target2"
        :
    else
        # echo "Symlink $link2 points to wrong target ($current_target2), removing and recreating"
        sudo rm "$link2"
        sudo ln -s "$target2" "$link2"
        # echo "Created symlink: $link2 -> $target2"
    fi
else
    # echo "Creating new symlink: $link2 -> $target2"
    sudo ln -s "$target2" "$link2"
fi

# 检查第三个符号链接
if [ -L "$link3" ]; then
    current_target3=$(readlink "$link3")
    if [ "$current_target3" = "$target3" ]; then
        # echo "Symlink $link3 already points to correct target: $target3"
        :
    else
        # echo "Symlink $link3 points to wrong target ($current_target3), removing and recreating"
        sudo rm "$link3"
        sudo ln -s "$target3" "$link3"
        # echo "Created symlink: $link3 -> $target3"
    fi
else
    # echo "Creating new symlink: $link3 -> $target3"
    sudo ln -s "$target3" "$link3"
fi

# 使用 qemu-riscv64-static 的 strace 模式执行程序
qemu-loongarch64-static "$program_path" 2>&1
echo --------------------------------------------------------
qemu-loongarch64-static -strace "$program_path" 2>&1
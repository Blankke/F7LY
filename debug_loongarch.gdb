# GDB 配置文件用于调试 LoongArch 内核
# loongarch64-linux-gnu-gdb build/loongarch/kernel.elf -x debug_loongarch.gdb

file tmp/ld-linux-loongarch-lp64d.so.1
set substitute-path /home/airxs/user/gnu/build-cross-tools-hf/glibc-2.38/ /home/kidszz/glibc-2.38
add-symbol-file /home/kidszz/F7LY/tmp/ld-linux-loongarch-lp64d.so.1 0x55c40
b *0x66a18
target remote localhost:1234
layout split
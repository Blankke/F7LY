# GDB 配置文件用于调试 LoongArch 内核
# loongarch64-linux-gnu-gdb build/loongarch/kernel.elf -x debug_loongarch.gdb
file build/loongarch/kernel.elf
target remote localhost:1235

# 设置断点
break _entry
break main

file busybox/loongarch/musl/busybox

layout split

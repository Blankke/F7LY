# GDB 配置文件用于调试 LoongArch 内核
# loongarch64-linux-gnu-gdb -x debug/gdb/loongarch-ref.gdb
file build/loongarch/kernel-la
target remote localhost:1234

# 设置断点
break _entry
break main

file busybox/loongarch/musl/busybox

layout split

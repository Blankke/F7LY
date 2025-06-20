# GDB 配置文件用于调试 LoongArch 内核
# loongarch64-linux-gnu-gdb build/loongarch/kernel.elf -x debug_loongarch.gdb
file build/loongarch/kernel.elf
target remote localhost:1234

# 设置断点
break _entry
break main

# 显示当前位置信息
info registers
x/10i $pc

layout split

echo "=== 调试会话已开始 ===\n"
echo "提示：使用 'c' 继续执行，'si' 单步执行，'ni' 执行到下一行\n"

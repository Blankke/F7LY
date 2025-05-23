file build/riscv/kernel.elf
set architecture riscv:rv64
target remote localhost:1234
b kernel/proc/proc_manager.cc:18
layout split
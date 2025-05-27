file build/riscv/kernel.elf
set architecture riscv:rv64
target remote localhost:1234
b kernel/trap/riscv/trap.cc:189
layout split
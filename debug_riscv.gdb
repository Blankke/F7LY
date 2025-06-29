file build/riscv/kernel.elf
set architecture riscv:rv64
target remote localhost:1234
b *0x25fe8
layout asm
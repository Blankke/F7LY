# RISC-V 内核调试入口；先运行 `make debug ARCH=riscv`。
file build/riscv/kernel-qemu
set architecture riscv:rv64
target remote localhost:1234
layout asm

# RISC-V 内核调试入口；先运行 `make debug PROFILE=riscv-qemu`。
file kernel-rv
set architecture riscv:rv64
target remote localhost:1234
layout asm

file build/riscv/kernel.elf
set architecture riscv:rv64
target remote localhost:1234
b kernel/fs/vfs/file/device_file.cc:31
layout split
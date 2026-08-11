# LoongArch 内核调试入口；先运行 `make debug PROFILE=loongarch-qemu`。
file kernel-la
target remote localhost:1234
layout split

# LoongArch 内核调试入口；先运行 `make debug ARCH=loongarch`。
file build/loongarch/kernel-la
target remote localhost:1234
layout split

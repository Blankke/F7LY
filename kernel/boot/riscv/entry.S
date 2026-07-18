.section .text.entry
.global _entry
#include "param.h"
# sbi load the hartid in a0
_entry:
        # keep each CPU's hartid in its tp register, for cpuid().
        # 在使用 hartid 计算启动栈前先做容量检查，避免 QEMU 配置超过内核
        # NCPU 上限时越界写 BSS。
        li t2, NCPU
        bgeu a0, t2, spin
        la sp, stack0
        li t0, 4096*2
        # load hartid
        mv t1, a0
        addi t1, t1, 1
        mul t0, t0, t1
        add sp, sp, t0
        # jump to start() in start.c
        call start
spin:
        j spin

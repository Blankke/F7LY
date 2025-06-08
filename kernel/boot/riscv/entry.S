.section .text.entry
.global _entry
# sbi load the hartid in a0
_entry:
        # keep each CPU's hartid in its tp register, for cpuid().
        la sp, stack0
        li t0, 1024*4
        # load hartid
        mv t1, a0
        addi t1, t1, 1
        mul t0, t0, t1
        add sp, sp, t0
        # jump to start() in start.c
        call start
spin:
        j spin

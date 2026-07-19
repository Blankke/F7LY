.section .text.entry
.global _entry
# sbi load the hartid in a0
_entry:
        # keep each CPU's hartid in its tp register, for cpuid().
        la sp, stack0

        # 裸 .bin 不包含 .bss；在跳入 C++ 代码前主动清零，避免物理板
        # 上 U-Boot 之后的内存残留破坏全局对象和锁的初始状态。
        la t2, bss_start
        la t3, bss_end
clear_bss:
        bgeu t2, t3, clear_bss_done
        sd zero, 0(t2)
        addi t2, t2, 8
        j clear_bss
clear_bss_done:
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

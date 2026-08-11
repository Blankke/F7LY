#include "asm.hh"
#include "param.h"

.section .text
.global _entry
_entry:
        # 固件入口存在多套 ABI：QEMU/UHI 常直接在 a1 传 DTB，U-Boot go
        # 则传 argc/argv。先完整保存 a0-a3，进入 C++ 后统一解析。
        move        $s0, $a0
        move        $s1, $a1
        move        $s2, $a2
        move        $s3, $a3
        li.d        $s4, 0                  # 主入口负责清理 BSS
        b           entry_common

.global _secondary_entry
_secondary_entry:
        # 次核由主核通过 IOCSR mailbox 唤醒，不应继承或重新解释主核的固件
        # 参数，更不能再次清空全局 BSS。使用独立入口明确这两个启动契约。
        move        $s0, $zero
        move        $s1, $zero
        move        $s2, $zero
        move        $s3, $zero
        li.d        $s4, 1

entry_common:

        # entry只对每个CPU设定自己的栈空间，然后跳转到main函数

        li.d        $t0, 0x8000000000000001
        csrwr       $t0, LOONGARCH_CSR_DMWIN0   # 设置设备操作空间
        li.d        $t0, (0x9000000000000001 | 1 << 4)
        csrwr       $t0, LOONGARCH_CSR_DMWIN1   # 设置指令数据访问空间
        // li.d        $t0, 0x0000000000000000
        // csrwr       $t0, LOONGARCH_CSR_DMWIN2
        // csrwr       $t0, LOONGARCH_CSR_DMWIN3
        csrwr       $t0, LOONGARCH_CSR_TLBRENTRY   # 关闭TLB，其他两个窗口不用管

        li.w	    $t0, 0xb0		      # PLV=0, IE=0, PG=1
	csrwr	    $t0, LOONGARCH_CSR_CRMD
        li.w        $t0, 0x00                 # PPLV=0, PIE=0, PWE=0
        csrwr       $t0, LOONGARCH_CSR_PRMD
        li.w        $t0, 0x01                 # FPE=1, SXE=0, ASXE=0, BTE=0
        csrwr       $t0, LOONGARCH_CSR_EUEN

        # 早期输出属于平台实现。QEMU 提供空实现，2K1000 提供有界 UART 标记；
        # LoongArch 公共入口不再包含板级 MMIO 地址。
        bnez        $s4, .Learly_uart_done
        bl          platform_early_boot_marker
.Learly_uart_done:

        # 主入口无条件清理普通 BSS，因此引导核不再被错误限定为 CPU0；次核
        # 从 _secondary_entry 到达并跳过本段。启动栈位于 sbss_clear 之前。
        bnez        $s4, bss_clear_done
        la.global   $t0, sbss_clear
        la.global   $t1, ebss_clear
bss_clear_loop:
        bgeu        $t0, $t1, bss_clear_done
        st.d        $zero, $t0, 0
        addi.d      $t0, $t0, 8
        b           bss_clear_loop
bss_clear_done:
        dbar        0

        // invtlb      0x0,$zero,$zero # 清除所有页表项
	# set up a stack for C.
        # 每个 CPU 使用独立的 8KB 启动栈。先检查 CPUID，避免超过 NCPU 时
        # 在进入 C 代码前已经越界写 boot_stack。

        csrrd       $a1, LOONGARCH_CSR_CPUID
        andi        $a1, $a1, 0x1ff
        li.d        $t2, NCPU
        bgeu        $a1, $t2, spin

        la.global   $sp, boot_stack
        li.d        $a0, 4096*2
        addi.d      $tp, $a1, 0                 # tp = CPUID
        addi.d      $a1, $a1, 1                 # a1 = CPUID + 1
        mul.d       $a0, $a0, $a1               # a0 = 8192 * (CPUID + 1)
        add.d       $sp, $sp, $a0               # sp = boot_stack + 8192 * (CPUID + 1)
	# jump to main in main.c
        li.d        $t0, 0x1
        cpucfg      $t1, $t0                    # read the CFG
        
        # 准备调用 main(hartid, fw_arg0, fw_arg1, fw_arg2, fw_arg3)：
        csrrd       $a0, LOONGARCH_CSR_CPUID    # $a0 = hartid (从 CPUID CSR 读取)
        andi        $a0, $a0, 0x1ff             # 取低9位作为 Core ID
        move        $a1, $s0
        move        $a2, $s1
        move        $a3, $s2
        move        $a4, $s3
        bl          main
spin:
        b           spin

    .section .bss.stack
    .align 12
    .globl boot_stack
boot_stack:
    .space 4096 * 2 * NCPU
    .globl boot_stack_top
boot_stack_top:

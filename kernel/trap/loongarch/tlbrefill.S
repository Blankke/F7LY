#ifdef LOONGARCH
#include "asm.hh"

.section tlbrentry
.globl handle_tlbr
.align 0x4
handle_tlbr:
	csrwr	$t0, LOONGARCH_CSR_TLBRSAVE
	csrrd	$t0, LOONGARCH_CSR_PGD
//	lddir	$t0, $t0, 4
//	addi.d  $t0, $t0, -1
	lddir	$t0, $t0, 3
	addi.d  $t0, $t0, -1
	lddir	$t0, $t0, 2
	addi.d  $t0, $t0, -1
	lddir   $t0, $t0, 1
	addi.d  $t0, $t0, -1
	ldpte	$t0, 0
	ldpte	$t0, 1
	tlbfill
	# TLB refill 也是以 ertn 收尾。若这次 refill 是在“用户态 LL 后、
	# 真正的 userret 之前”的异常窗口里发生，直接 ertn 会把 LLBit 清掉，
	# 用户态随后那条 SC 就会永远失败。这里和 userret 一样，只按位置位 KLO，
	# 不覆盖 LLBCTL 里的其他硬件状态位。
	li.d	$t0, 0x4
	csrxchg $t0, $t0, LOONGARCH_CSR_LLBCTL
	csrrd	$t0, LOONGARCH_CSR_TLBRSAVE
	ertn
	
#endif

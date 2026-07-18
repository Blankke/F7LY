#ifdef LOONGARCH
#include "asm.hh"

.section tlbrentry
.globl handle_tlbr
.align 0x4
handle_tlbr:
	csrwr	$t0, LOONGARCH_CSR_TLBRSAVE
	csrrd	$t0, LOONGARCH_CSR_PGD
	lddir	$t0, $t0, 3
	// F7LY 的非叶目录项编码为“下级页表物理地址 | PTE_V(bit0)”。
	// LDDIR 返回的是该原始目录项；后续 LDDIR/LDPTE 需要纯页表基址。
	// 因而在每层间去除 F7LY 的唯一目录标志位。Starry 的目录项编码不同，
	// 不能直接照搬其无此转换的写法。
	addi.d	$t0, $t0, -1
	lddir	$t0, $t0, 2
	addi.d	$t0, $t0, -1
	lddir   $t0, $t0, 1
	addi.d	$t0, $t0, -1
	ldpte	$t0, 0
	ldpte	$t0, 1
	tlbfill
	csrrd	$t0, LOONGARCH_CSR_TLBRSAVE
	ertn
	
#endif


#ifdef LOONGARCH
#pragma once

// This file defines the register names and values for the LoongArch architecture.
#ifndef _LOONGARCHREGS_H
#define _LOONGARCHREGS_H

/* LoongArch Registers */
#define REG_RA	0x1
#define REG_TP	0x2
#define REG_SP	0x3
#define REG_A0	0x4
#define REG_A1	0x5
#define REG_A2	0x6
#define REG_A3	0x7
#define REG_A4	0x8
#define REG_A5	0x9
#define REG_A6	0xa
#define REG_A7	0xb
#define REG_V0	REG_A0
#define REG_V1	REG_A1
#define REG_T0	0xc
#define REG_T1	0xd
#define REG_T2	0xe
#define REG_T3	0xf
#define REG_T4	0x10
#define REG_T5	0x11
#define REG_T6	0x12
#define REG_T7	0x13
#define REG_T8	0x14
#define REG_U0	0x15
#define REG_FP	0x16
#define REG_S0	0x17
#define REG_S1	0x18
#define REG_S2	0x19
#define REG_S3	0x1a
#define REG_S4	0x1b
#define REG_S5	0x1c
#define REG_S6	0x1d
#define REG_S7	0x1e
#define REG_S8	0x1f

/* LoongArch Control and Status Registers */
#define LOONGARCH_CSR_CRMD		    0x0	    /* Current mode info 当前模式信息 */
#define LOONGARCH_CSR_PRMD          0x1	    /* Previous mode info 例外前模式信息 */
#define LOONGARCH_CSR_EUEN		    0x2	    /* Extension unit enable 扩展部件使能 */
#define LOONGARCH_CSR_MISC          0x3	    /* Miscellaneous control 杂项控制 */
#define LOONGARCH_CSR_ECFG          0x4	    /* Exception config 例外配置 */
#define LOONGARCH_CSR_ESTAT         0x5	    /* Exception status 例外状态 */
#define LOONGARCH_CSR_ERAISE        0x6	    /* Exception return address 例外返回地址 */
#define LOONGARCH_CSR_BADV          0x7	    /* Bad virtual address 错误虚拟地址 */
#define LOONGARCH_CSR_BADI          0x8	    /* Bad instruction 错误指令 */
#define LOONGARCH_CSR_EENTRY        0x9	    /* Exception entry address 例外入口地址 */

#define LOONGARCH_CSR_CPUID		    0x20	/* CPU core id */
// 上下文保存寄存器

#define LOONGARCH_CSR_PRCFG1		0x21	/* Privilege config 1 */

#define LOONGARCH_CSR_PRCFG2		0x22	/* Config2 */
#define LOONGARCH_CSR_PRCFG3		0x23	/* Config3 */

#define LOONGARCH_CSR_SAVE0		    0x30    /* Kscratch registers */

#define LOONGARCH_CSR_TID   		0x40	/* Timer ID */
#define LOONGARCH_CSR_TCFG          0x41    /* Timer config */

#define LOONGARCH_CSR_DMWIN0		0x180	/* 64 direct map win0: MEM & IF */
#define LOONGARCH_CSR_DMWIN1		0x181	/* 64 direct map win1: MEM & IF */
#define LOONGARCH_CSR_DMWIN2		0x182	/* 64 direct map win2: MEM */
#define LOONGARCH_CSR_DMWIN3		0x183	/* 64 direct map win3: MEM */

#define LOONGARCH_CSR_TLBEHI		0x11	/* TLB EntryHi */
#define LOONGARCH_CSR_PGDL          0x19
#define LOONGARCH_CSR_PGDH          0x1a
#define LOONGARCH_CSR_PGD           0x1b
#define LOONGARCH_CSR_PWCL          0x1c
#define LOONGARCH_CSR_PWCH          0x1d

#define LOONGARCH_CSR_TLBRENTRY		0x88	/* TLB refill exception entry */
#define LOONGARCH_CSR_TLBRBADV		0x89	/* TLB refill badvaddr */
#define LOONGARCH_CSR_TLBRSAVE		0x8b	/* KScratch for TLB refill exception */

/* Direct Map window 0/1 */

#endif /* _LOONGARCHREGS_H */
#endif
#pragma once
#include "types.hh"

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2015 Regents of the University of California
 */

#ifndef _ASM_RISCV_SBI_H
#define _ASM_RISCV_SBI_H

#define SBI_SET_TIMER 0
#define SBI_CONSOLE_PUTCHAR 1
#define SBI_CONSOLE_GETCHAR 2
#define SBI_CLEAR_IPI 3
#define SBI_SEND_IPI 4
#define SBI_REMOTE_FENCE_I 5
#define SBI_REMOTE_SFENCE_VMA 6
#define SBI_REMOTE_SFENCE_VMA_ASID 7
#define SBI_SHUTDOWN 8

#define SBI_EXT_HSM 0x48534D
#define SBI_EXT_HSM_HART_START 0x0

// 定义SBI调用返回结构体
struct sbiret {
  uint64 error;
  uint64 value;
};

#define SBI_CALL(which, arg0, arg1, arg2, arg3) ({		\
	register uintptr_t a0 asm ("a0") = (uintptr_t)(arg0);	\
	register uintptr_t a1 asm ("a1") = (uintptr_t)(arg1);	\
	register uintptr_t a2 asm ("a2") = (uintptr_t)(arg2);	\
	register uintptr_t a3 asm ("a3") = (uintptr_t)(arg3);	\
	register uintptr_t a7 asm ("a7") = (uintptr_t)(which);	\
	asm volatile ("ecall"					\
		      : "+r" (a0)				\
		      : "r" (a1), "r" (a2), "r" (a3), "r" (a7)	\
		      : "memory");				\
	a0;							\
})

/* Lazy implementations until SBI is finalized */
#define SBI_CALL_0(which) SBI_CALL(which, 0, 0, 0, 0)
#define SBI_CALL_1(which, arg0) SBI_CALL(which, arg0, 0, 0, 0)
#define SBI_CALL_2(which, arg0, arg1) SBI_CALL(which, arg0, arg1, 0, 0)
#define SBI_CALL_3(which, arg0, arg1, arg2) \
		SBI_CALL(which, arg0, arg1, arg2, 0)
#define SBI_CALL_4(which, arg0, arg1, arg2, arg3) \
		SBI_CALL(which, arg0, arg1, arg2, arg3)

static inline struct sbiret new_sbi_call(uint64 eid, uint64 fid, uint64 arg0, uint64 arg1, uint64 arg2, uint64 arg3, uint64 arg4, uint64 arg5) {
  struct sbiret ret;
  register uint64 a0 asm("a0") = arg0;
  register uint64 a1 asm("a1") = arg1;
  register uint64 a2 asm("a2") = arg2;
  register uint64 a3 asm("a3") = arg3;
  register uint64 a4 asm("a4") = arg4;
  register uint64 a5 asm("a5") = arg5;
  register uint64 a6 asm("a6") = fid;
  register uint64 a7 asm("a7") = eid;
  asm volatile("ecall"
    : "+r" (a0), "+r" (a1)
    : "r" (a2), "r" (a3), "r" (a4), "r" (a5), "r" (a6), "r" (a7)
    : "memory"
  );
  ret.error = a0;
  ret.value = a1;
  return ret;
}

static inline void sbi_console_putchar(int ch)
{
	SBI_CALL_1(SBI_CONSOLE_PUTCHAR, ch);
}

static inline int sbi_console_getchar(void)
{
	return SBI_CALL_0(SBI_CONSOLE_GETCHAR);
}

static inline void sbi_set_timer(uint64 stime_value)
{
	SBI_CALL_1(SBI_SET_TIMER, stime_value);
}

static inline void sbi_shutdown(void)
{
	SBI_CALL_0(SBI_SHUTDOWN);
}

static inline void sbi_clear_ipi(void)
{
	SBI_CALL_0(SBI_CLEAR_IPI);
}

static inline void sbi_send_ipi(const unsigned long *hart_mask)
{
	SBI_CALL_1(SBI_SEND_IPI, hart_mask);
}

static inline void sbi_remote_fence_i(const unsigned long *hart_mask)
{
	SBI_CALL_1(SBI_REMOTE_FENCE_I, hart_mask);
}

static inline void sbi_remote_sfence_vma(const unsigned long *hart_mask,
					 unsigned long start,
					 unsigned long size)
{
	SBI_CALL_3(SBI_REMOTE_SFENCE_VMA, hart_mask, start, size);
}

static inline void sbi_remote_sfence_vma_asid(const unsigned long *hart_mask,
					      unsigned long start,
					      unsigned long size,
					      unsigned long asid)
{
	SBI_CALL_4(SBI_REMOTE_SFENCE_VMA_ASID, hart_mask, start, size, asid);
}

static inline void sbi_set_extern_interrupt(unsigned long func_pointer) {
	asm volatile("mv a6, %0" : : "r" (0x210));
	SBI_CALL_1(0x0A000004, func_pointer);
}

static inline void sbi_set_mie(void) {
	SBI_CALL_0(0x0A000005);
}

static inline int sbi_hart_start(uint64 hartid, uint64 start_addr, uint64 opaque) {
  struct sbiret ret;
  ret = new_sbi_call(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hartid, start_addr, opaque, 0, 0, 0);
  if(ret.error)
    return ret.error;
  return ret.value;
}

#endif

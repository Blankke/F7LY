#pragma once

#include "types.hh"

// RISC-V CSR 位定义。这里只描述指令集机制，不保存 QEMU/开发板 MMIO 地址。
#define MSTATUS_MPP_MASK (3L << 11)
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3)

#define SSTATUS_SPP (1L << 8)
#define SSTATUS_SPIE (1L << 5)
#define SSTATUS_UPIE (1L << 4)
#define SSTATUS_SIE (1L << 1)
#define SSTATUS_UIE (1L << 0)

#define SIE_SEIE (1L << 9)
#define SIE_STIE (1L << 5)
#define SIE_SSIE (1L << 1)

#define MIE_MEIE (1L << 11)
#define MIE_MTIE (1L << 7)
#define MIE_MSIE (1L << 3)

static inline uint64 r_mhartid()
{
    uint64 value;
    asm volatile("csrr %0, mhartid" : "=r"(value));
    return value;
}

static inline uint64 r_mstatus()
{
    uint64 value;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static inline void w_mstatus(uint64 value)
{
    asm volatile("csrw mstatus, %0" : : "r"(value));
}

static inline void w_mepc(uint64 value)
{
    asm volatile("csrw mepc, %0" : : "r"(value));
}

static inline uint64 r_sstatus()
{
    uint64 value;
    asm volatile("csrr %0, sstatus" : "=r"(value));
    return value;
}

static inline void w_sstatus(uint64 value)
{
    asm volatile("csrw sstatus, %0" : : "r"(value));
}

static inline uint64 r_sip()
{
    uint64 value;
    asm volatile("csrr %0, sip" : "=r"(value));
    return value;
}

static inline void w_sip(uint64 value)
{
    asm volatile("csrw sip, %0" : : "r"(value));
}

static inline uint64 r_sie()
{
    uint64 value;
    asm volatile("csrr %0, sie" : "=r"(value));
    return value;
}

static inline void w_sie(uint64 value)
{
    asm volatile("csrw sie, %0" : : "r"(value));
}

static inline uint64 r_mie()
{
    uint64 value;
    asm volatile("csrr %0, mie" : "=r"(value));
    return value;
}

static inline void w_mie(uint64 value)
{
    asm volatile("csrw mie, %0" : : "r"(value));
}

static inline void w_sepc(uint64 value)
{
    asm volatile("csrw sepc, %0" : : "r"(value));
}

static inline uint64 r_sepc()
{
    uint64 value;
    asm volatile("csrr %0, sepc" : "=r"(value));
    return value;
}

static inline uint64 r_medeleg()
{
    uint64 value;
    asm volatile("csrr %0, medeleg" : "=r"(value));
    return value;
}

static inline void w_medeleg(uint64 value)
{
    asm volatile("csrw medeleg, %0" : : "r"(value));
}

static inline uint64 r_mideleg()
{
    uint64 value;
    asm volatile("csrr %0, mideleg" : "=r"(value));
    return value;
}

static inline void w_mideleg(uint64 value)
{
    asm volatile("csrw mideleg, %0" : : "r"(value));
}

static inline void w_stvec(uint64 value)
{
    asm volatile("csrw stvec, %0" : : "r"(value));
}

static inline uint64 r_stvec()
{
    uint64 value;
    asm volatile("csrr %0, stvec" : "=r"(value));
    return value;
}

static inline void w_mtvec(uint64 value)
{
    asm volatile("csrw mtvec, %0" : : "r"(value));
}

static inline void w_pmpcfg0(uint64 value)
{
    asm volatile("csrw pmpcfg0, %0" : : "r"(value));
}

static inline void w_pmpaddr0(uint64 value)
{
    asm volatile("csrw pmpaddr0, %0" : : "r"(value));
}

static inline void w_satp(uint64 value)
{
    asm volatile("csrw satp, %0" : : "r"(value));
}

static inline uint64 r_satp()
{
    uint64 value;
    asm volatile("csrr %0, satp" : "=r"(value));
    return value;
}

static inline void w_mscratch(uint64 value)
{
    asm volatile("csrw mscratch, %0" : : "r"(value));
}

static inline uint64 r_scause()
{
    uint64 value;
    asm volatile("csrr %0, scause" : "=r"(value));
    return value;
}

static inline uint64 r_stval()
{
    uint64 value;
    asm volatile("csrr %0, stval" : "=r"(value));
    return value;
}

static inline void w_mcounteren(uint64 value)
{
    asm volatile("csrw mcounteren, %0" : : "r"(value));
}

static inline uint64 r_mcounteren()
{
    uint64 value;
    asm volatile("csrr %0, mcounteren" : "=r"(value));
    return value;
}

static inline uint64 r_time()
{
    uint64 value;
    asm volatile("csrr %0, time" : "=r"(value));
    return value;
}

static inline void intr_on()
{
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}

static inline void intr_off()
{
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

static inline int intr_get()
{
    return (r_sstatus() & SSTATUS_SIE) != 0;
}

static inline uint64 r_sp()
{
    uint64 value;
    asm volatile("mv %0, sp" : "=r"(value));
    return value;
}

static inline uint64 r_fp()
{
    uint64 value;
    asm volatile("mv %0, s0" : "=r"(value));
    return value;
}

static inline uint64 r_tp()
{
    uint64 value;
    asm volatile("mv %0, tp" : "=r"(value));
    return value;
}

static inline void w_tp(uint64 value)
{
    asm volatile("mv tp, %0" : : "r"(value));
}

static inline uint64 r_ra()
{
    uint64 value;
    asm volatile("mv %0, ra" : "=r"(value));
    return value;
}

[[maybe_unused]] static inline uint64 rdtime()
{
    uint64 value;
    asm volatile("rdtime %0" : "=r"(value));
    return value;
}

static inline void sfence_vma()
{
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

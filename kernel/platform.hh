#pragma once

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "memlayout.hh"
#ifdef RISCV

#ifndef __ASSEMBLER__
#include "types.hh"

// which hart (core) is this?
static inline uint64
r_mhartid()
{
  uint64 x;
  asm volatile("csrr %0, mhartid" : "=r"(x));
  return x;
}

// Machine Status Register, mstatus

#define MSTATUS_MPP_MASK (3L << 11) // previous mode.
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3) // machine-mode interrupt enable.

static inline uint64
r_mstatus()
{
  uint64 x;
  asm volatile("csrr %0, mstatus" : "=r"(x));
  return x;
}

static inline void
w_mstatus(uint64 x)
{
  asm volatile("csrw mstatus, %0" : : "r"(x));
}

// machine exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void
w_mepc(uint64 x)
{
  asm volatile("csrw mepc, %0" : : "r"(x));
}

// Supervisor Status Register, sstatus

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4) // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)  // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)  // User Interrupt Enable

static inline uint64
r_sstatus()
{
  uint64 x;
  asm volatile("csrr %0, sstatus" : "=r"(x));
  return x;
}

static inline void
w_sstatus(uint64 x)
{
  asm volatile("csrw sstatus, %0" : : "r"(x));
}

// Supervisor Interrupt Pending
static inline uint64
r_sip()
{
  uint64 x;
  asm volatile("csrr %0, sip" : "=r"(x));
  return x;
}

static inline void
w_sip(uint64 x)
{
  asm volatile("csrw sip, %0" : : "r"(x));
}

// Supervisor Interrupt Enable
#define SIE_SEIE (1L << 9) // external
#define SIE_STIE (1L << 5) // timer
#define SIE_SSIE (1L << 1) // software
static inline uint64
r_sie()
{
  uint64 x;
  asm volatile("csrr %0, sie" : "=r"(x));
  return x;
}

static inline void
w_sie(uint64 x)
{
  asm volatile("csrw sie, %0" : : "r"(x));
}

// Machine-mode Interrupt Enable
#define MIE_MEIE (1L << 11) // external
#define MIE_MTIE (1L << 7)  // timer
#define MIE_MSIE (1L << 3)  // software
static inline uint64
r_mie()
{
  uint64 x;
  asm volatile("csrr %0, mie" : "=r"(x));
  return x;
}

static inline void
w_mie(uint64 x)
{
  asm volatile("csrw mie, %0" : : "r"(x));
}

// supervisor exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void
w_sepc(uint64 x)
{
  asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline uint64
r_sepc()
{
  uint64 x;
  asm volatile("csrr %0, sepc" : "=r"(x));
  return x;
}

// Machine Exception Delegation
static inline uint64
r_medeleg()
{
  uint64 x;
  asm volatile("csrr %0, medeleg" : "=r"(x));
  return x;
}

static inline void
w_medeleg(uint64 x)
{
  asm volatile("csrw medeleg, %0" : : "r"(x));
}

// Machine Interrupt Delegation
static inline uint64
r_mideleg()
{
  uint64 x;
  asm volatile("csrr %0, mideleg" : "=r"(x));
  return x;
}

static inline void
w_mideleg(uint64 x)
{
  asm volatile("csrw mideleg, %0" : : "r"(x));
}

// Supervisor Trap-Vector Base Address
// low two bits are mode.
static inline void
w_stvec(uint64 x)
{
  asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline uint64
r_stvec()
{
  uint64 x;
  asm volatile("csrr %0, stvec" : "=r"(x));
  return x;
}

// Machine-mode interrupt vector
static inline void
w_mtvec(uint64 x)
{
  asm volatile("csrw mtvec, %0" : : "r"(x));
}

// Physical Memory Protection
static inline void
w_pmpcfg0(uint64 x)
{
  asm volatile("csrw pmpcfg0, %0" : : "r"(x));
}

static inline void
w_pmpaddr0(uint64 x)
{
  asm volatile("csrw pmpaddr0, %0" : : "r"(x));
}

// use riscv's sv39 page table scheme.
#define SATP_SV39 (8L << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// supervisor address translation and protection;
// holds the address of the page table.
static inline void
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r"(x));
}

static inline uint64
r_satp()
{
  uint64 x;
  asm volatile("csrr %0, satp" : "=r"(x));
  return x;
}

static inline void
w_mscratch(uint64 x)
{
  asm volatile("csrw mscratch, %0" : : "r"(x));
}

// Supervisor Trap Cause
static inline uint64
r_scause()
{
  uint64 x;
  asm volatile("csrr %0, scause" : "=r"(x));
  return x;
}

// Supervisor Trap Value
static inline uint64
r_stval()
{
  uint64 x;
  asm volatile("csrr %0, stval" : "=r"(x));
  return x;
}

// Machine-mode Counter-Enable
static inline void
w_mcounteren(uint64 x)
{
  asm volatile("csrw mcounteren, %0" : : "r"(x));
}

static inline uint64
r_mcounteren()
{
  uint64 x;
  asm volatile("csrr %0, mcounteren" : "=r"(x));
  return x;
}

// machine-mode cycle counter
static inline uint64
r_time()
{
  uint64 x;
  asm volatile("csrr %0, time" : "=r"(x));
  return x;
}

// enable device interrupts
static inline void
intr_on()
{
  w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// disable device interrupts
static inline void
intr_off()
{
  w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

// are device interrupts enabled?
static inline int
intr_get()
{
  uint64 x = r_sstatus();
  return (x & SSTATUS_SIE) != 0;
}

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mv %0, sp" : "=r"(x));
  return x;
}

static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r"(x));
  return x;
}

// read and write tp, the thread pointer, which xv6 uses to hold
// this core's hartid (core number), the index into cpus[].
static inline uint64
r_tp()
{
  uint64 x;
  asm volatile("mv %0, tp" : "=r"(x));
  return x;
}

static inline void
w_tp(uint64 x)
{
  asm volatile("mv tp, %0" : : "r"(x));
}

static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mv %0, ra" : "=r"(x));
  return x;
}

///@todo 以后记得打开注释，现在没用这个函数
[[maybe_unused]]static uint64 rdtime() {
  uint64 x;
  asm volatile("rdtime %0" : "=r"(x));
  return x;
}

// flush the TLB.
static inline void
sfence_vma()
{
  // the zero, zero means flush all TLB entries.
  asm volatile("sfence.vma zero, zero");
}

typedef uint64 pte_t;
typedef uint64 *pagetable_t; // 512 PTEs

#endif // __ASSEMBLER__

#define PGSIZE 4096 // bytes per page
#define PGSHIFT 12  // bits of offset within a page

// PGROUNDUP(sz)：sz大小的内存至少使用多少页才可以存下，返回的是下一个未使用页的地址
// PGROUNDDOWN(a)：地址a所在页面是多少号页面，拉回所在页面开始地址
#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access

// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK 0x1FF // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// one beyond the highest possible virtual address.
// MAXVA is actually one bit less than the max allowed by
// Sv39, to avoid having to sign-extend virtual addresses
// that have the high bit set.
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1)) // 128TB
enum vml : uint64
{
  // vm_page_cnt_shift = 15,
  // vm_start = 0x0,
  // vm_end = ( 0x1UL << PGSHIFT ) << vm_page_cnt_shift,	// 128 MiB

  // vm_kernel_start = vm_end >> 2,								// 32 MiB
  // vm_trap_frame = ( vm_end >> 1 ) - PGSIZE,					// 64 MiB - 1 page

  // vm_kernel_heap_start = vm_kernel_start >> 1,
  // vm_kernel_heap_start = _1M * 8,
  vm_kernel_heap_start = MAXVA >> 1, // 64 MiB
  vm_kernel_heap_size = _1M*384
};
#define HEAP_START PHYSTOP - vm_kernel_heap_size
#elif defined(LOONGARCH)

#define DISK_IRQ 1
// #include <larchintrin.h>
#include "types.hh"
#define CSR_CRMD_IE_SHIFT 2
#define CSR_CRMD_IE (0x1 << CSR_CRMD_IE_SHIFT)

#define EXT_INT_EN_SHIFT 48

// #define LOONGARCH_IOCSR_EXTIOI_EN_BASE		    0x1600
// #define LOONGARCH_IOCSR_EXTIOI_ISR_BASE		    0x1800
// #define LOONGARCH_IOCSR_EXTIOI_MAP_BASE       0x14c0
// #define LOONGARCH_IOCSR_EXTIOI_ROUTE_BASE	    0x1c00
// #define LOONGARCH_IOCSR_EXRIOI_NODETYPE_BASE  0x14a0

// read and write tp, the thread pointer, which holds
// this core's hartid (core number), the index into cpus[].

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("addi.d %0, $sp, 0" : "=r"(x));
  return x;
}

static inline uint64
r_tp()
{
  uint64 x;
  asm volatile("addi.d %0, $tp, 0" : "=r"(x));
  return x;
}

static inline uint32
r_csr_crmd()
{
  uint32 x;
  asm volatile("csrrd %0, 0x0" : "=r"(x));
  return x;
}

static inline void
w_csr_crmd(uint32 x)
{
  asm volatile("csrwr %0, 0x0" : : "r"(x));
}

#define PRMD_PPLV (3U << 0) // Previous Privilege
#define PRMD_PIE (1U << 2)  // Previous Int_enable

static inline uint32
r_csr_prmd()
{
  uint32 x;
  asm volatile("csrrd %0, 0x1" : "=r"(x));
  return x;
}

static inline void
w_csr_prmd(uint32 x)
{
  asm volatile("csrwr %0, 0x1" : : "r"(x));
}

static inline uint64
r_csr_era()
{
  uint64 x;
  asm volatile("csrrd %0, 0x6" : "=r"(x));
  return x;
}

static inline void
w_csr_era(uint64 x)
{
  asm volatile("csrwr %0, 0x6" : : "r"(x));
}

#define CSR_ESTAT_ECODE (0x3fU << 16)

static inline uint32
r_csr_estat()
{
  uint32 x;
  asm volatile("csrrd %0, 0x5" : "=r"(x));
  return x;
}

#define CSR_ECFG_VS_SHIFT 16
#define CSR_ECFG_LIE_TI_SHIFT 11
#define HWI_VEC 0x3fcU
#define TI_VEC (0x1 << CSR_ECFG_LIE_TI_SHIFT)

static inline uint32
r_csr_ecfg()
{
  uint32 x;
  asm volatile("csrrd %0, 0x4" : "=r"(x));
  return x;
}

static inline void
w_csr_ecfg(uint32 x)
{
  asm volatile("csrwr %0, 0x4" : : "r"(x));
}

#define CSR_TICLR_CLR (0x1 << 0)

static inline uint32
r_csr_ticlr()
{
  uint32 x;
  asm volatile("csrrd %0, 0x44" : "=r"(x));
  return x;
}

static inline void
w_csr_ticlr(uint32 x)
{
  asm volatile("csrwr %0, 0x44" : : "r"(x));
}

static inline uint64
r_csr_eentry()
{
  uint64 x;
  asm volatile("csrrd %0, 0xc" : "=r"(x));
  return x;
}

static inline uint64
r_csr_tlbrelo0()
{
  uint64 x;
  asm volatile("csrrd %0, 0x8c" : "=r"(x));
  return x;
}

static inline uint64
r_csr_tlbrelo1()
{
  uint64 x;
  asm volatile("csrrd %0, 0x8d" : "=r"(x));
  return x;
}

static inline void
w_csr_eentry(uint64 x)
{
  asm volatile("csrwr %0, 0xc" : : "r"(x));
}

static inline void
w_csr_tlbrentry(uint64 x)
{
  asm volatile("csrwr %0, 0x88" : : "r"(x));
}

static inline void
w_csr_merrentry(uint64 x)
{
  asm volatile("csrwr %0, 0x93" : : "r"(x));
}

static inline void
w_csr_stlbps(uint32 x)
{
  asm volatile("csrwr %0, 0x1e" : : "r"(x));
}

static inline void
w_csr_asid(uint32 x)
{
  asm volatile("csrwr %0, 0x18" : : "r"(x));
}

#define CSR_TCFG_EN (1U << 0)
#define CSR_TCFG_PER (1U << 1)

static inline void
w_csr_tcfg(uint64 x)
{
  asm volatile("csrwr %0, 0x41" : : "r"(x));
}

static inline void
w_csr_tlbrehi(uint64 x)
{
  asm volatile("csrwr %0, 0x8e" : : "r"(x));
}

static inline uint64
r_csr_pgdl()
{
  uint64 x;
  asm volatile("csrrd %0, 0x19" : "=r"(x));
  return x;
}

static inline void
w_csr_pgdl(uint64 x)
{
  asm volatile("csrwr %0, 0x19" : : "r"(x));
}

static inline void
w_csr_pgdh(uint64 x)
{
  asm volatile("csrwr %0, 0x1a" : : "r"(x));
}

static inline uint64
r_csr_tval( )
{
  uint64 x;
  asm volatile("rdtime.d %0, $zero" : "=r"(x));
  return x;
}



#define PTBASE 12U
#define PTWIDTH 9U
#define DIR1BASE 21U
#define DIR1WIDTH 9U
#define DIR2BASE 30U
#define DIR2WIDTH 9U
#define PTEWIDTH 0U
#define DIR3BASE 39U
#define DIR3WIDTH 9U
#define DIR4WIDTH 0U

#define PWCH_D3BASE 39
#define PWCH_D3WIDRH 9 << 6
#define PWCH_D4BASE 0 << 12
#define PWCH_D4WIDTH 0 << 18
#define PWCH_HPTW_EN 1 << 24

static inline void
w_csr_pwcl(uint32 x)
{
  asm volatile("csrwr %0, 0x1c" : : "r"(x));
}

static inline void
w_csr_pwch(uint32 x)
{
  asm volatile("csrwr %0, 0x1d" : : "r"(x));
}

static inline uint32
r_csr_badi()
{
  uint32 x;
  asm volatile("csrrd %0, 0x8" : "=r"(x));
  return x;
}

static inline uint64
r_csr_badv()
{
  uint64 x;
  asm volatile("csrrd %0, 0x7" : "=r"(x));
  return x;
}

static inline void
w_csr_euen(uint32 x)
{
  asm volatile("csrwr %0, 0x2" : : "r"(x));
}

static inline uint32
r_csr_euen()
{
  uint32 x;
  asm volatile("csrrd %0, 0x2" : "=r"(x));
  return x;
}

[[maybe_unused]]static uint64 rdtime(void) {
  int rID = 0;
  uint64 val = 0;

  asm volatile(
    "rdtime.d %0, %1 \n\t"
    : "=r" (val) , "=r" (rID)
    :
  );
  return val;
}
// static uint64 rdtime(void) {
//   int rID = 0;
//   uint64 val = 0;

//   asm volatile(
//     "rdtime.d %0, %1 \n\t"
//     : "=r" (val) , "=r" (rID)
//     :
//   );
//   return val;
// }

/* IOCSR */
constexpr uint64 LOONGARCH_IOCSR_EXTIOI_EN_BASE = 0x1600 | DMWIN1_MASK;
constexpr uint64 LOONGARCH_IOCSR_EXTIOI_ISR_BASE = 0x1800 | DMWIN1_MASK;
constexpr uint64 LOONGARCH_IOCSR_EXTIOI_MAP_BASE = 0x14c0 | DMWIN1_MASK;
constexpr uint64 LOONGARCH_IOCSR_EXTIOI_ROUTE_BASE = 0x1c00 | DMWIN1_MASK;
constexpr uint64 LOONGARCH_IOCSR_EXRIOI_NODETYPE_BASE = 0x14a0 | DMWIN1_MASK;
constexpr uint64 dmwin_mask = 0xFUL << 60;
constexpr uint64 virt_to_phy_address(uint64 virt) { return virt & ~dmwin_mask; }
inline void write_itr_cfg(uint64 itrReg, uint32 data)
{
  *((volatile uint32 *)itrReg) = data;
}
inline uint32 read_itr_cfg(uint64 itrReg)
{
  return *((volatile uint32 *)itrReg);
}
inline void write_itr_cfg_64b(uint64 itrReg, uint64 data)
{
  *((volatile uint64 *)itrReg) = data;
}
inline uint64 read_itr_cfg_64b(uint64 itrReg)
{
  return *((volatile uint64 *)itrReg);
}

inline void write_itr_cfg_8b(uint64 itr_reg, u8 data)
{
  *(volatile u8 *)itr_reg = data;
}
inline u8 read_itr_cfg_8b(uint64 itr_reg)
{
  return *((volatile u8 *)itr_reg);
}
static inline int
intr_get()
{
  uint32 x = r_csr_crmd();
  return (x & CSR_CRMD_IE) != 0;
}
constexpr uint64 iodma_win_base = 0x0UL << 60;
constexpr uint64 iodma_win_mask = ~(0x0UL) << 32;


inline ulong to_phy(ulong addr) { return addr & ~dmwin_mask; }
inline ulong to_vir(ulong addr) { return to_phy(addr) | DMWIN_MASK; }
inline ulong to_io(ulong addr) { return to_phy(addr) | DMWIN1_MASK; }
inline ulong to_dma(ulong addr) { return to_phy(addr) | iodma_win_base; }

// enable device interrupts
static inline void
intr_on()
{
  w_csr_crmd(r_csr_crmd() | CSR_CRMD_IE);
}

// disable device interrupts
static inline void
intr_off()
{
  w_csr_crmd(r_csr_crmd() & ~CSR_CRMD_IE);
}

#define PGSIZE 4096 // bytes per page
#define PGSHIFT 12  // bits of offset within a page

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1L << 0)   // valid
#define PTE_D (1L << 1)   // dirty
#define PTE_PLV (3L << 2) // privilege level
#define PTE_U (3L << 2)
#define PTE_MAT (1L << 4)    // memory access type
#define PTE_P (1L << 7)      // physical page exists
#define PTE_W (1L << 8)      // writeable
#define PTE_NX (1UL << 62)   // non executable
#define PTE_NR (1L << 61)    // non readable
#define PTE_RPLV (1UL << 63) // restricted privilege level enable
#define PTE_R 0
#define PTE_X 0

#define PAMASK 0xFFFFFFFFFUL << PGSHIFT
#define PTE2PA(pte) (pte & PAMASK)
// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) (((uint64)pa) & PAMASK)
#define PTE_FLAGS(pte) ((pte) & 0xE0000000000001FFUL)

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK 0x1FF // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

#define MAXVA (1ULL << (9 + 9 + 9 + 9 + 12 - 2))

#define dsb() __sync_synchronize()       // For virtio-blk-pci

typedef uint64 pte_t;
typedef uint64 *pagetable_t;

// vm_page_cnt_shift = 15,
// vm_start = 0x0,
// vm_end = ( 0x1UL << PGSHIFT ) << vm_page_cnt_shift,	// 128 MiB

// vm_kernel_start = vm_end >> 2,								// 32 MiB
// vm_trap_frame = ( vm_end >> 1 ) - PGSIZE,					// 64 MiB - 1 page

// vm_kernel_heap_start = vm_kernel_start >> 1,
// vm_kernel_heap_start = _1M * 8,
enum vml : uint64
{
  // vm_page_cnt_shift = 15,
  // vm_start = 0x0,
  // vm_end = ( 0x1UL << PGSHIFT ) << vm_page_cnt_shift,	// 128 MiB

  // vm_kernel_start = vm_end >> 2,								// 32 MiB
  // vm_trap_frame = ( vm_end >> 1 ) - PGSIZE,					// 64 MiB - 1 page

  // vm_kernel_heap_start = vm_kernel_start >> 1,
  // vm_kernel_heap_start = _1M * 8,
  vm_kernel_heap_start = MAXVA >> 1, // 64 MiB
  vm_kernel_heap_size = _1M * 24,
};
#define HEAP_START PHYSTOP - vm_kernel_heap_size

#endif

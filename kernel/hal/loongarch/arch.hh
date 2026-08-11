#pragma once

#include "hal/loongarch/platform_board.hh"
#include "types.hh"

// LoongArch DMW 是架构地址转换机制，不是具体开发板的 MMIO 资源。
inline constexpr uint64 DMWIN_MASK = 0x9ULL << 60;
inline constexpr uint64 DMWIN1_MASK = 0x8ULL << 60;
inline constexpr uint64 VIRT_DMWIN_MASK = 0xf000000000000000ULL;
inline constexpr uint64 PHYSBASE = DMWIN_MASK;

#define VIRT2PHY(address) (loongarch::board::physical_address((address)))
#define PA2VA(address) ((address) & ~DMWIN_MASK)

inline ulong to_phy(ulong address)
{
    return loongarch::board::physical_address(address);
}

inline ulong to_vir(ulong address)
{
    return loongarch::board::cached_address(address);
}

inline ulong to_io(ulong address)
{
    return loongarch::board::mmio_address(address);
}

inline ulong to_dma(ulong address)
{
    return loongarch::board::physical_address(address);
}

// CPU 状态寄存器位定义。
#define CSR_CRMD_IE_SHIFT 2
#define CSR_CRMD_IE (1U << CSR_CRMD_IE_SHIFT)
#define EXT_INT_EN_SHIFT 48

#define PRMD_PPLV (3U << 0)
#define PRMD_PIE (1U << 2)

#define CSR_ESTAT_ECODE (0x3fU << 16)
#define CSR_ECFG_VS_SHIFT 16
#define CSR_ECFG_LIE_TI_SHIFT 11
#define HWI_VEC 0x3fcU
#define TI_VEC (1U << CSR_ECFG_LIE_TI_SHIFT)
#define IPI_VEC (1U << 12)
#define CSR_TICLR_CLR (1U << 0)
#define CSR_TCFG_EN (1U << 0)
#define CSR_TCFG_PER (1U << 1)

static inline uint64 r_sp()
{
    uint64 value;
    asm volatile("addi.d %0, $sp, 0" : "=r"(value));
    return value;
}

static inline uint64 r_tp()
{
    uint64 value;
    asm volatile("addi.d %0, $tp, 0" : "=r"(value));
    return value;
}

static inline void w_tp(uint64 value)
{
    asm volatile("addi.d $tp, %0, 0" : : "r"(value));
}

static inline uint64 r_ra()
{
    uint64 value;
    asm volatile("addi.d %0, $ra, 0" : "=r"(value));
    return value;
}

static inline uint32 r_csr_cpuid()
{
    uint32 value;
    asm volatile("csrrd %0, 0x20" : "=r"(value));
    return value;
}

static inline uint32 r_csr_crmd()
{
    uint32 value;
    asm volatile("csrrd %0, 0x0" : "=r"(value));
    return value;
}

static inline void w_csr_crmd(uint32 value)
{
    asm volatile("csrwr %0, 0x0" : : "r"(value));
}

static inline uint32 r_csr_prmd()
{
    uint32 value;
    asm volatile("csrrd %0, 0x1" : "=r"(value));
    return value;
}

static inline void w_csr_prmd(uint32 value)
{
    asm volatile("csrwr %0, 0x1" : : "r"(value));
}

static inline uint64 r_csr_era()
{
    uint64 value;
    asm volatile("csrrd %0, 0x6" : "=r"(value));
    return value;
}

static inline void w_csr_era(uint64 value)
{
    asm volatile("csrwr %0, 0x6" : : "r"(value));
}

static inline uint32 r_csr_estat()
{
    uint32 value;
    asm volatile("csrrd %0, 0x5" : "=r"(value));
    return value;
}

static inline uint32 r_csr_ecfg()
{
    uint32 value;
    asm volatile("csrrd %0, 0x4" : "=r"(value));
    return value;
}

static inline void w_csr_ecfg(uint32 value)
{
    asm volatile("csrwr %0, 0x4" : : "r"(value));
}

static inline uint32 r_csr_ticlr()
{
    uint32 value;
    asm volatile("csrrd %0, 0x44" : "=r"(value));
    return value;
}

static inline void w_csr_ticlr(uint32 value)
{
    asm volatile("csrwr %0, 0x44" : : "r"(value));
}

static inline uint64 r_csr_eentry()
{
    uint64 value;
    asm volatile("csrrd %0, 0xc" : "=r"(value));
    return value;
}

static inline uint64 r_csr_save1()
{
    uint64 value;
    asm volatile("csrrd %0, 0x31" : "=r"(value));
    return value;
}

static inline uint64 r_csr_save2()
{
    uint64 value;
    asm volatile("csrrd %0, 0x32" : "=r"(value));
    return value;
}

static inline uint64 r_csr_tlbrelo0()
{
    uint64 value;
    asm volatile("csrrd %0, 0x8c" : "=r"(value));
    return value;
}

static inline uint64 r_csr_tlbrelo1()
{
    uint64 value;
    asm volatile("csrrd %0, 0x8d" : "=r"(value));
    return value;
}

static inline void w_csr_eentry(uint64 value)
{
    asm volatile("csrwr %0, 0xc" : : "r"(value));
}

static inline void w_csr_tlbrentry(uint64 value)
{
    asm volatile("csrwr %0, 0x88" : : "r"(value));
}

static inline void w_csr_merrentry(uint64 value)
{
    asm volatile("csrwr %0, 0x93" : : "r"(value));
}

static inline void w_csr_stlbps(uint32 value)
{
    asm volatile("csrwr %0, 0x1e" : : "r"(value));
}

static inline void w_csr_asid(uint32 value)
{
    asm volatile("csrwr %0, 0x18" : : "r"(value));
}

static inline void w_csr_tcfg(uint64 value)
{
    asm volatile("csrwr %0, 0x41" : : "r"(value));
}

static inline void w_csr_tlbrehi(uint64 value)
{
    asm volatile("csrwr %0, 0x8e" : : "r"(value));
}

static inline uint64 r_csr_pgdl()
{
    uint64 value;
    asm volatile("csrrd %0, 0x19" : "=r"(value));
    return value;
}

static inline void w_csr_pgdl(uint64 value)
{
    asm volatile("csrwr %0, 0x19" : : "r"(value));
}

static inline void w_csr_pgdh(uint64 value)
{
    asm volatile("csrwr %0, 0x1a" : : "r"(value));
}

static inline uint64 r_csr_tval()
{
    uint64 value;
    asm volatile("rdtime.d %0, $zero" : "=r"(value));
    return value;
}

// 四级页表遍历寄存器编码；配置值由 VMM 在开启分页前统一写入。
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
#define PWCH_HPTW_EN 1U

static inline void w_csr_pwcl(uint32 value)
{
    asm volatile("csrwr %0, 0x1c" : : "r"(value));
}

static inline void w_csr_pwch(uint32 value)
{
    asm volatile("csrwr %0, 0x1d" : : "r"(value));
}

static inline uint32 r_csr_badi()
{
    uint32 value;
    asm volatile("csrrd %0, 0x8" : "=r"(value));
    return value;
}

static inline uint64 r_csr_badv()
{
    uint64 value;
    asm volatile("csrrd %0, 0x7" : "=r"(value));
    return value;
}

static inline void w_csr_euen(uint32 value)
{
    asm volatile("csrwr %0, 0x2" : : "r"(value));
}

static inline uint32 r_csr_euen()
{
    uint32 value;
    asm volatile("csrrd %0, 0x2" : "=r"(value));
    return value;
}

[[maybe_unused]] static inline uint64 rdtime()
{
    uint64 value;
    uint32 timer_id;
    asm volatile("rdtime.d %0, %1" : "=r"(value), "=r"(timer_id));
    return value;
}

static inline int intr_get()
{
    return (r_csr_crmd() & CSR_CRMD_IE) != 0;
}

static inline void intr_on()
{
    w_csr_crmd(r_csr_crmd() | CSR_CRMD_IE);
}

static inline void intr_off()
{
    w_csr_crmd(r_csr_crmd() & ~CSR_CRMD_IE);
}

// LoongArch 的 dbar 0 足以约束当前 VirtIO MMIO/PCI 队列访问顺序。
static inline void dsb()
{
    asm volatile("dbar 0" ::: "memory");
}

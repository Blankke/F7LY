#pragma once

#include "mem/page.hh"
#include "types.hh"

namespace riscv
{
    // Sv39 页表项硬件位；RSW 位由上层内存管理器另行分配给软件状态。
    enum PteEnum : uint64
    {
        pte_valid_m = 1ULL << 0,
        pte_readable_m = 1ULL << 1,
        pte_writable_m = 1ULL << 2,
        pte_executable_m = 1ULL << 3,
        pte_user_m = 1ULL << 4,
        pte_global_m = 1ULL << 5,
        pte_accessed_m = 1ULL << 6,
        pte_dirty_m = 1ULL << 7,
    };

    inline constexpr uint64 k_physical_address_mask = 0x0000ffffffffffffULL;

    constexpr uint64 virt_to_phy_address(uint64 address)
    {
        return address & k_physical_address_mask;
    }

    inline constexpr uint64 k_satp_sv39 = 8ULL << 60;
    inline constexpr uint64 k_satp_asid_shift = 44;
    inline constexpr uint64 k_satp_asid_mask = 0xffffULL;

    constexpr uint64 make_satp(uint64 page_table, uint64 asid = 0)
    {
        return k_satp_sv39 | (page_table >> PGSHIFT) |
               ((asid & k_satp_asid_mask) << k_satp_asid_shift);
    }
} // namespace riscv

#define PTE_V riscv::pte_valid_m
#define PTE_R riscv::pte_readable_m
#define PTE_W riscv::pte_writable_m
#define PTE_X riscv::pte_executable_m
#define PTE_U riscv::pte_user_m

#define PA2PTE(address) ((((uint64)(address)) >> PGSHIFT) << 10)
#define PTE2PA(pte) ((((uint64)(pte)) >> 10) << PGSHIFT)
#define PTE_FLAGS(pte) ((pte) & 0x3ffULL)

#define PXMASK 0x1ffULL
#define PXSHIFT(level) (PGSHIFT + (9U * (level)))
#define PX(level, address) ((((uint64)(address)) >> PXSHIFT(level)) & PXMASK)

// Sv39 的最高半区需要符号扩展；当前用户地址空间只使用低 38 位。
inline constexpr uint64 MAXVA = 1ULL << 38;

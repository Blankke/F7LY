#pragma once

#include "hal/loongarch/platform_board.hh"
#include "mem/page.hh"
#include "types.hh"

namespace loongarch
{
    inline constexpr uint64 pte_valid_m = 1ULL << 0;
    inline constexpr uint64 pte_dirty_m = 1ULL << 1;
    inline constexpr uint64 pte_plv_s = 2;
    inline constexpr uint64 pte_plv_m = 3ULL << pte_plv_s;
    inline constexpr uint64 pte_mat_s = 4;
    inline constexpr uint64 pte_mat_m = 3ULL << pte_mat_s;
    inline constexpr uint64 pte_b_global_m = 1ULL << 6;
    inline constexpr uint64 pte_presence_m = 1ULL << 7;
    inline constexpr uint64 pte_writable_m = 1ULL << 8;
    inline constexpr uint64 pte_cow_m = 1ULL << 9;
    inline constexpr uint64 pte_nr_m = 1ULL << 61;
    inline constexpr uint64 pte_nx_m = 1ULL << 62;
    inline constexpr uint64 pte_rplv_m = 1ULL << 63;

    inline constexpr uint64 pte_b_flags_m =
        pte_valid_m | pte_dirty_m | pte_plv_m | pte_mat_m |
        pte_b_global_m | pte_presence_m | pte_writable_m |
        pte_nr_m | pte_nx_m | pte_rplv_m;

    namespace csr::crmd
    {
        inline constexpr uint32 ie_m = 1U << 2;
    }
} // namespace loongarch

#define PTE_V loongarch::pte_valid_m
#define PTE_D loongarch::pte_dirty_m
#define PTE_PLV loongarch::pte_plv_m
#define PTE_U loongarch::pte_plv_m
#define PTE_MAT (1ULL << 4)
#define PTE_P loongarch::pte_presence_m
#define PTE_W loongarch::pte_writable_m
#define PTE_COW loongarch::pte_cow_m
#define PTE_NX loongarch::pte_nx_m
#define PTE_NR loongarch::pte_nr_m
#define PTE_RPLV loongarch::pte_rplv_m
#define PTE_R 0ULL
#define PTE_X 0ULL

#define PAMASK (0xfffffffffULL << PGSHIFT)
#define PTE2PA(pte) ((pte) & PAMASK)
#define PA2PTE(address) (((uint64)(address)) & PAMASK)
#define PTE_FLAGS(pte) ((pte) & 0xe0000000000003ffULL)

#define PXMASK 0x1ffULL
#define PXSHIFT(level) (PGSHIFT + (9U * (level)))
#define PX(level, address) ((((uint64)(address)) >> PXSHIFT(level)) & PXMASK)

// 可用用户虚拟地址宽度由所选机器画像声明，页表编码本身不识别板名。
inline constexpr uint64 MAXVA = 1ULL << loongarch::board::k_user_virtual_address_bits;

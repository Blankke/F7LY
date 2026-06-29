#pragma once
#ifdef LOONGARCH

#include "types.hh"
#include "platform.hh"

// LoongArch TLB 操作统一 helper。
// 这一阶段先保持本仓库既有失效语义，只把散落的内联汇编收口到统一入口；
// 后续若参考 starry-next/arceos 减少全局 flush，应在单独阶段验证。
// - invtlb 0x0 全刷 TLB（仅 PGDL 根切换、进程地址空间变更时使用）。
// - invtlb 0x6 按页对（相邻双页 pair base）失效，用于单页/相邻页变更。
// - 所有 TLB 操作前补 dbar 0 屏障，保证此前内存操作全局可见后再刷 TLB。

#ifdef LOONGARCH_TLB_STATS
// 可选 TLB 统计（编译时定义 LOONGARCH_TLB_STATS=1 启用）。
namespace mem::loongarch::stats
{
    inline uint64 flush_all_count = 0;
    inline uint64 flush_page_count = 0;
    inline uint64 pgdl_switch_count = 0;
}
#define LOONGARCH_TLB_STAT_INC(var) ++mem::loongarch::stats::var
#else
#define LOONGARCH_TLB_STAT_INC(var) ((void)0)
#endif

namespace mem::loongarch
{

/// 全刷 TLB（所有项失效）。仅在 PGDL 根切换或进程地址空间替换时调用。
inline void tlb_flush_all()
{
    LOONGARCH_TLB_STAT_INC(flush_all_count);
    asm volatile("dbar 0" : : : "memory");
    asm volatile("invtlb 0x0, $zero, $zero" : : : "memory");
}

/// 按页对失效 TLB。va 需为用户虚地址，内部自动对齐到 pair base（相邻双页）。
inline void tlb_flush_user_page_pair(uint64 va)
{
    LOONGARCH_TLB_STAT_INC(flush_page_count);
    uint64 pair_base = va & ~((PGSIZE << 1) - 1);
    asm volatile("dbar 0" : : : "memory");
    asm volatile("invtlb 0x6, $zero, %0" : : "r"(pair_base) : "memory");
}

/// 写入 PGDL 并在根切换时全刷 TLB。
/// 仅当 new_pgdl 与当前 PGDL 不同时才执行写入 + 全刷；
/// 相同则跳过，避免重复写根和全局 flush。
inline void write_pgdl_and_flush_if_changed(uint64 new_pgdl)
{
    uint64 cur = r_csr_pgdl();
    if (cur != new_pgdl)
    {
        LOONGARCH_TLB_STAT_INC(pgdl_switch_count);
        w_csr_pgdl(new_pgdl);
        tlb_flush_all();
    }
}

} // namespace mem::loongarch

#endif // LOONGARCH

/**
 * @file tlb_mm.cc
 * @brief 地址空间级用户 ASID 生命周期管理。
 *
 * 使用示例：
 *   make build PROFILE=riscv-qemu
 *
 * ASID 随 ProcessMemoryManager 创建和销毁；CLONE_VM 只增加 mm 引用，不重新
 * 分配 ASID。耗尽时仍保留一次全核失效作为回收屏障，但正常 mmap/mprotect/
 * COW 路径不再进入这里。
 */

#include "process_memory_manager.hh"
#include "proc.hh"
#include "hal/tlb_shootdown.hh"
#include "param.h"
#include "spinlock.hh"

namespace proc
{
#if defined(RISCV) || defined(LOONGARCH)
namespace
{
    constexpr uint32 k_user_asid_count = 1U << 10;
    constexpr uint32 k_first_user_asid = 1;
    static_assert(num_process < k_user_asid_count,
                  "用户 ASID 数量必须覆盖全部 PCB 槽位");

    SpinLock g_user_asid_lock;
    bool g_user_asid_active[k_user_asid_count]{};
    bool g_user_asid_retired[k_user_asid_count]{};
    uint64 g_user_asid_retired_epoch[k_user_asid_count]{};
    uint64 g_user_asid_retirement_epoch = 0;
    bool g_user_asid_reclaiming = false;
    uint32 g_next_user_asid = k_first_user_asid;
}

void initialize_user_asid_allocator()
{
    g_user_asid_lock.init("user_asid");
    memset(g_user_asid_active, 0, sizeof(g_user_asid_active));
    memset(g_user_asid_retired, 0, sizeof(g_user_asid_retired));
    memset(g_user_asid_retired_epoch, 0, sizeof(g_user_asid_retired_epoch));
    g_user_asid_retirement_epoch = 0;
    g_user_asid_reclaiming = false;
    g_next_user_asid = k_first_user_asid;
}

uint32 allocate_user_asid()
{
    for (;;)
    {
        g_user_asid_lock.acquire();
        for (uint32 offset = 0; offset < k_user_asid_count - 1; ++offset)
        {
            const uint32 asid =
                k_first_user_asid +
                ((g_next_user_asid - k_first_user_asid + offset) %
                 (k_user_asid_count - 1));
            if (g_user_asid_active[asid] || g_user_asid_retired[asid])
            {
                continue;
            }

            g_user_asid_active[asid] = true;
            g_next_user_asid = asid + 1 < k_user_asid_count
                                   ? asid + 1
                                   : k_first_user_asid;
            g_user_asid_lock.release();
            return asid;
        }

        if (g_user_asid_reclaiming)
        {
            g_user_asid_lock.release();
            hal::tlb::poll_pending();
            asm volatile("nop");
            continue;
        }

        g_user_asid_reclaiming = true;
        const uint64 reclaim_epoch = g_user_asid_retirement_epoch;
        g_user_asid_lock.release();

        // 只有 ASID 耗尽才允许全核屏障；普通地址空间更新走 mm 定向路径。
        hal::tlb::flush_all_cpus();

        g_user_asid_lock.acquire();
        for (uint32 asid = k_first_user_asid;
             asid < k_user_asid_count;
             ++asid)
        {
            if (!g_user_asid_active[asid] &&
                g_user_asid_retired[asid] &&
                g_user_asid_retired_epoch[asid] <= reclaim_epoch)
            {
                g_user_asid_retired[asid] = false;
                g_user_asid_retired_epoch[asid] = 0;
            }
        }
        g_user_asid_reclaiming = false;
        g_user_asid_lock.release();
    }
}

void retire_user_asid(uint32 asid)
{
    if (asid < k_first_user_asid || asid >= k_user_asid_count)
    {
        return;
    }
    g_user_asid_lock.acquire();
    g_user_asid_active[asid] = false;
    g_user_asid_retired[asid] = true;
    g_user_asid_retired_epoch[asid] = ++g_user_asid_retirement_epoch;
    g_user_asid_lock.release();
}
#else
void initialize_user_asid_allocator() {}
uint32 allocate_user_asid() { return 0; }
void retire_user_asid(uint32) {}
#endif
}

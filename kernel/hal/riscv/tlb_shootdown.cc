#include "hal/tlb_shootdown.hh"

#include "hal/cpu.hh"
#include "hal/riscv/sbi.hh"
#include "mem/memlayout.hh"
#include "proc/process_memory_manager.hh"
#include "libs/perf_diag.hh"

namespace hal::tlb
{
namespace
{
    // 仅由对应 hart 读写自己的槽位。指针非空表示该 mm 的 active 位已经
    // 发布且遗漏代际已经补齐；scheduler/exec 切出地址空间时由 leave_mm 清空。
    proc::ProcessMemoryManager *g_current_mm[NUMCPU]{};

    bool normalize_range(uint64 start, uint64 size,
                         uint64 &normalized_start, uint64 &normalized_size)
    {
        if (size == 0 || start > ~0ULL - size || start + size > ~0ULL - (PGSIZE - 1))
        {
            normalized_start = 0;
            normalized_size = 0;
            return false;
        }
        normalized_start = PGROUNDDOWN(start);
        const uint64 normalized_end = PGROUNDUP(start + size);
        normalized_size = normalized_end - normalized_start;
        return true;
    }

    void flush_local_asid(uint32 asid, uint64 start, uint64 size)
    {
        uint64 normalized_start = 0;
        uint64 normalized_size = 0;
        if (asid == 0 ||
            !normalize_range(start, size, normalized_start, normalized_size) ||
            normalized_size > PGSIZE * 64)
        {
            // ASID 定向的全量失效不会影响其它地址空间的翻译。
            asm volatile("sfence.vma zero, %0" : : "r"(static_cast<uint64>(asid)) : "memory");
            return;
        }

        const uint64 end = normalized_start + normalized_size;
        for (uint64 address = normalized_start; address < end; address += PGSIZE)
        {
            asm volatile("sfence.vma %0, %1"
                         :
                         : "r"(address), "r"(static_cast<uint64>(asid))
                         : "memory");
        }
    }

    // 与每次 trap 返回的同-mm 指针比较分离，避免慢路径需要的寄存器保存
    // 污染已命中时的函数序言/尾声。
    __attribute__((noinline)) void enter_mm_slow(proc::ProcessMemoryManager &mm,
                                                 uint64 cpu_id)
    {
        const uint64 cpu_bit = 1ULL << cpu_id;
        for (;;)
        {
            mm.tlb_state_lock.acquire();
            mm.tlb_active_cpu_mask |= cpu_bit;
            const uint64 generation = mm.tlb_generation;
            const uint64 seen = mm.tlb_seen_generation[cpu_id];
            mm.tlb_state_lock.release();
            if (seen >= generation)
            {
                return;
            }

            flush_local_asid(mm.user_asid, 0, 0);
            mm.tlb_state_lock.acquire();
            if (mm.tlb_seen_generation[cpu_id] < generation)
            {
                mm.tlb_seen_generation[cpu_id] = generation;
            }
            const bool caught_up =
                mm.tlb_seen_generation[cpu_id] >= mm.tlb_generation;
            mm.tlb_state_lock.release();
            if (caught_up)
            {
                return;
            }
        }
    }

}

void initialize_current_cpu()
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (Cpu::is_valid_cpu_id(cpu_id))
    {
        g_current_mm[cpu_id] = nullptr;
    }
    flush_local_range(0, 0);
}

void flush_local_range(uint64 start, uint64 size)
{
    F7LY_PERF_ADD(TlbFlush, 1);
    uint64 normalized_start = 0;
    uint64 normalized_size = 0;
    if (!normalize_range(start, size, normalized_start, normalized_size) ||
        normalized_size > PGSIZE * 64)
    {
        F7LY_PERF_ADD(TlbFullFlush, 1);
        asm volatile("sfence.vma zero, zero" ::: "memory");
        return;
    }

    const uint64 end = normalized_start + normalized_size;
    for (uint64 address = normalized_start; address < end; address += PGSIZE)
    {
        asm volatile("sfence.vma %0, zero" : : "r"(address) : "memory");
    }
}

void flush_range_all_cpus(uint64 start, uint64 size)
{
    // 先使 PTE 写入在本 hart 可见，再执行本地和远端同步失效。legacy SBI
    // remote_sfence_vma 返回时，目标 hart 已完成 fence。
    asm volatile("fence rw, rw" ::: "memory");
    flush_local_range(start, size);

    const uint64 current_cpu = Cpu::current_cpu_id();
    uint64 remote_mask = Cpu::online_cpu_mask();
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        remote_mask &= ~(1ULL << current_cpu);
    }
    if (remote_mask == 0)
    {
        return;
    }
    F7LY_PERF_ADD(TlbRemoteCpu, perfdiag::count_set_bits(remote_mask));

    uint64 remote_start = 0;
    uint64 remote_size = 0;
    normalize_range(start, size, remote_start, remote_size);
    unsigned long hart_mask = static_cast<unsigned long>(remote_mask);
    sbi_remote_sfence_vma(&hart_mask,
                          static_cast<unsigned long>(remote_start),
                          static_cast<unsigned long>(remote_size));
}

void flush_all_cpus()
{
    flush_range_all_cpus(0, 0);
}

void enter_mm(proc::ProcessMemoryManager &mm)
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (cpu_id >= NUMCPU)
    {
        return;
    }
    if (g_current_mm[cpu_id] == &mm)
    {
        // 地址空间保持 active 时，所有 PTE 更新都会同步 shootdown 本 CPU；
        // 因此重复的 trap 返回无需再次争用 mm.tlb_state_lock。
        return;
    }
    enter_mm_slow(mm, cpu_id);
    g_current_mm[cpu_id] = &mm;
}

void leave_mm(proc::ProcessMemoryManager &mm)
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (!Cpu::is_valid_cpu_id(cpu_id))
    {
        return;
    }
    if (g_current_mm[cpu_id] == &mm)
    {
        g_current_mm[cpu_id] = nullptr;
    }
    mm.tlb_state_lock.acquire();
    mm.tlb_active_cpu_mask &= ~(1ULL << cpu_id);
    mm.tlb_state_lock.release();
}

void flush_mm_range(proc::ProcessMemoryManager &mm, uint64 start, uint64 size)
{
    mm.tlb_flush_lock.acquire();
    asm volatile("fence rw, rw" ::: "memory");

    const uint64 current_cpu = Cpu::current_cpu_id();
    uint64 generation = 0;
    uint64 remote_mask = 0;
    mm.tlb_state_lock.acquire();
    generation = ++mm.tlb_generation;
    remote_mask = mm.tlb_active_cpu_mask;
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        remote_mask &= ~(1ULL << current_cpu);
    }
    mm.tlb_state_lock.release();

    flush_local_asid(mm.user_asid, start, size);
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        mm.tlb_state_lock.acquire();
        if (mm.tlb_seen_generation[current_cpu] < generation)
        {
            mm.tlb_seen_generation[current_cpu] = generation;
        }
        mm.tlb_state_lock.release();
    }
    if (remote_mask != 0)
    {
        F7LY_PERF_ADD(TlbRemoteCpu, perfdiag::count_set_bits(remote_mask));
        uint64 remote_start = 0;
        uint64 remote_size = 0;
        normalize_range(start, size, remote_start, remote_size);
        unsigned long hart_mask = static_cast<unsigned long>(remote_mask);
        sbi_remote_sfence_vma_asid(
            &hart_mask,
            static_cast<unsigned long>(remote_start),
            static_cast<unsigned long>(remote_size),
            static_cast<unsigned long>(mm.user_asid));
    }
    mm.tlb_flush_lock.release();
}

void poll_pending()
{
    // OpenSBI 在 M-mode 完成 remote fence，不依赖 S-mode 软件 IPI 分发。
}
}

#include "hal/tlb_shootdown.hh"

#include "hal/cpu.hh"
#include "hal/riscv/sbi.hh"
#include "mem/memlayout.hh"
#include "proc/process_memory_manager.hh"

namespace hal::tlb
{
namespace
{
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

}

void initialize_current_cpu()
{
    flush_local_range(0, 0);
}

void flush_local_range(uint64 start, uint64 size)
{
    uint64 normalized_start = 0;
    uint64 normalized_size = 0;
    if (!normalize_range(start, size, normalized_start, normalized_size) ||
        normalized_size > PGSIZE * 64)
    {
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
    if (!Cpu::is_valid_cpu_id(cpu_id))
    {
        return;
    }

    for (;;)
    {
        mm.tlb_state_lock.acquire();
        mm.tlb_active_cpu_mask |= 1ULL << cpu_id;
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
        const bool caught_up = mm.tlb_seen_generation[cpu_id] >= mm.tlb_generation;
        mm.tlb_state_lock.release();
        if (caught_up)
        {
            return;
        }
    }
}

void leave_mm(proc::ProcessMemoryManager &mm)
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (!Cpu::is_valid_cpu_id(cpu_id))
    {
        return;
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

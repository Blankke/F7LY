#include "hal/tlb_shootdown.hh"

#include "hal/cpu.hh"
#include "hal/riscv/sbi.hh"
#include "mem/memlayout.hh"

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

void poll_pending()
{
    // OpenSBI 在 M-mode 完成 remote fence，不依赖 S-mode 软件 IPI 分发。
}
}

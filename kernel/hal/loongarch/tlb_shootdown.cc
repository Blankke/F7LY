#include "hal/tlb_shootdown.hh"

#include "hal/cpu.hh"
#include "mem/memlayout.hh"
#include "param.h"
#include "printer.hh"
#include <EASTL/atomic.h>

namespace hal::tlb
{
namespace
{
    constexpr uint64 k_iocsr_ipi_status = 0x1000;
    constexpr uint64 k_iocsr_ipi_enable = 0x1004;
    constexpr uint64 k_iocsr_ipi_clear = 0x100c;
    constexpr uint64 k_iocsr_ipi_send = 0x1040;
    constexpr uint32 k_ipi_tlb_vector = 1;
    constexpr uint32 k_ipi_tlb_mask = 1U << k_ipi_tlb_vector;
    constexpr uint32 k_ipi_target_cpu_shift = 16;
    constexpr uint32 k_iocsr_send_blocking = 1U << 31;
    // LoongArch QEMU virt 的稳定计时器为 100 MHz。重发是协议的一部分：
    // IOCSR IPI 状态是可合并位，不能把一次边沿当作可靠消息队列。
    constexpr uint64 k_ipi_retry_cycles = 100000ULL;
    constexpr uint64 k_shootdown_timeout_cycles = 1000000000ULL;

    eastl::atomic<uint64> g_generation{0};
    eastl::atomic<uint64> g_requested_generation[NCPU]{};
    eastl::atomic<uint64> g_acknowledged_generation[NCPU]{};

    inline uint32 read_iocsr_word(uint64 address)
    {
        uint32 value = 0;
        asm volatile("iocsrrd.w %0, %1" : "=r"(value) : "r"(address) : "memory");
        return value;
    }

    inline void write_iocsr_word(uint64 address, uint32 value)
    {
        asm volatile("iocsrwr.w %0, %1" : : "r"(value), "r"(address) : "memory");
    }

    inline void send_tlb_ipi(uint64 cpu_id)
    {
        // 与 Linux LoongArch 的 ipi_write_action() 一致，使用 BLOCKING 保证
        // 这次跨核 IOCSR 写已经到达中断控制器后再继续等待 acknowledgement。
        asm volatile("dbar 0" ::: "memory");
        const uint32 value = static_cast<uint32>(k_iocsr_send_blocking |
                                                  (cpu_id << k_ipi_target_cpu_shift) |
                                                  k_ipi_tlb_vector);
        write_iocsr_word(k_iocsr_ipi_send, value);
        asm volatile("dbar 0" ::: "memory");
    }

    void publish_request(uint64 cpu_id, uint64 generation)
    {
        uint64 observed = g_requested_generation[cpu_id].load(eastl::memory_order_acquire);
        while (observed < generation &&
               !g_requested_generation[cpu_id].compare_exchange_weak(
                   observed, generation, eastl::memory_order_acq_rel))
        {
        }
    }
}

void initialize_current_cpu()
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (!Cpu::is_valid_cpu_id(cpu_id))
    {
        panic("[tlb] invalid LoongArch cpu id=%lu", cpu_id);
    }

    // 清掉 slave boot ROM 可能遗留的 vector0，再开放 runtime TLB vector。
    const uint32 pending = read_iocsr_word(k_iocsr_ipi_status);
    if (pending != 0)
    {
        write_iocsr_word(k_iocsr_ipi_clear, pending);
    }
    uint32 enabled = read_iocsr_word(k_iocsr_ipi_enable);
    write_iocsr_word(k_iocsr_ipi_enable, enabled | k_ipi_tlb_mask);

    const uint64 generation = g_generation.load(eastl::memory_order_acquire);
    g_requested_generation[cpu_id].store(generation, eastl::memory_order_release);
    flush_local_range(0, 0);
    g_acknowledged_generation[cpu_id].store(generation, eastl::memory_order_release);
}

void flush_local_range(uint64 start, uint64 size)
{
    (void)start;
    (void)size;
    // 跨核页表更新和 PCB/ASID 槽位回收仍以正确性优先，保守清空本 CPU
    // 全 TLB，避免相邻双页项以及任意用户 ASID 的历史翻译遗漏。
    asm volatile("dbar 0" ::: "memory");
    asm volatile("invtlb 0x0, $zero, $zero" ::: "memory");
    asm volatile("dbar 0" ::: "memory");
}

bool handle_ipi()
{
    const uint32 status = read_iocsr_word(k_iocsr_ipi_status);
    const uint64 cpu_id = Cpu::current_cpu_id();
    const uint64 requested =
        g_requested_generation[cpu_id].load(eastl::memory_order_acquire);
    const uint64 acknowledged =
        g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire);
    const bool has_tlb_status = (status & k_ipi_tlb_mask) != 0;

    // poll_pending() 在关中断自旋路径调用。即使硬件的可合并状态位已经被
    // 另一代请求清除，只要内存邮箱仍显示未确认请求，本核也必须完成它。
    if (!has_tlb_status && requested <= acknowledged)
    {
        return false;
    }

    if (has_tlb_status)
    {
        // 先清状态再重新读取请求。若新请求在清除后到达，硬件会再次置位；
        // 若它在清除前已合并，本次处理会直接确认最新 generation。
        write_iocsr_word(k_iocsr_ipi_clear, k_ipi_tlb_mask);
        asm volatile("dbar 0" ::: "memory");
    }
    const uint64 generation =
        g_requested_generation[cpu_id].load(eastl::memory_order_acquire);
    if (generation >
        g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire))
    {
        flush_local_range(0, 0);
        g_acknowledged_generation[cpu_id].store(generation, eastl::memory_order_release);
    }
    return true;
}

void poll_pending()
{
    handle_ipi();
}

void flush_range_all_cpus(uint64 start, uint64 size)
{
    // 明确把普通 PTE 写排在 request 发布之前；不能依赖随后才执行的本地
    // invtlb 来替远端 CPU 建立可见性顺序。
    asm volatile("dbar 0" ::: "memory");
    const uint64 current_cpu = Cpu::current_cpu_id();
    const uint64 online_mask = Cpu::online_cpu_mask();
    const uint64 generation = g_generation.fetch_add(1, eastl::memory_order_acq_rel) + 1;
    uint64 remote_mask = online_mask;
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        remote_mask &= ~(1ULL << current_cpu);
    }

    // PTE 写入先于 request 的 release；目标 CPU 在 acquire 后失效并回写 ack。
    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((remote_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }
        publish_request(cpu_id, generation);
        send_tlb_ipi(cpu_id);
    }

    flush_local_range(start, size);

    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((remote_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }
        const uint64 wait_start = Cpu::get_cpu()->get_time();
        uint64 next_retry = wait_start + k_ipi_retry_cycles;
        uint32 probe_spins = 0;
        while (g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire) < generation)
        {
            asm volatile("nop");
            // shootdown 常从 trap/系统调用路径发起，此时本核中断可能关闭。
            // 主动处理对端同时发来的请求，避免 A 等 B、B 等 A 的环形等待。
            // IOCSR 是设备访问，不能在每次 nop 后读取；固定间隔探测既保留
            // 环形等待的前进保证，也避免 mmap 密集负载被 MMIO 轮询拖垮。
            if ((++probe_spins & 0xffU) != 0)
            {
                continue;
            }
            poll_pending();

            const uint64 now = Cpu::get_cpu()->get_time();
            if (static_cast<int64>(now - next_retry) >= 0)
            {
                // request generation 单调递增，因此重复置同一 IPI 位完全幂等；
                // 周期性重发可从状态位合并或 vCPU 暂停窗口中可靠恢复。
                send_tlb_ipi(cpu_id);
                next_retry = now + k_ipi_retry_cycles;
            }
            if (now - wait_start >= k_shootdown_timeout_cycles)
            {
                panic("[tlb] LoongArch shootdown timeout sender=%lu target=%lu generation=%lu ack=%lu",
                      current_cpu, cpu_id, generation,
                      g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire));
            }
        }
    }
}

void flush_all_cpus()
{
    flush_range_all_cpus(0, 0);
}
}

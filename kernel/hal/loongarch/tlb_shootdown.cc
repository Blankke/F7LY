#include "hal/tlb_shootdown.hh"

#include "hal/cpu.hh"
#include "mem/memlayout.hh"
#include "proc/process_memory_manager.hh"
#include "param.h"
#include "printer.hh"
#include "tm/time.hh"
#include <EASTL/atomic.h>
#include "libs/perf_diag.hh"

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
    // 重发是协议的一部分：IOCSR IPI 状态是可合并位，不能把一次边沿当作
    // 可靠消息队列。策略使用明确时间单位，硬件周期由当前画像的 clock
    // backend 换算，不能把 QEMU 100MHz 的裸周期复制到 2K1000。
    constexpr uint64 k_ipi_retry_microseconds = 1'000ULL;
    constexpr uint64 k_shootdown_timeout_microseconds = 10'000'000ULL;

    eastl::atomic<uint64> g_generation{0};
    eastl::atomic<uint64> g_requested_generation[NCPU]{};
    eastl::atomic<uint64> g_acknowledged_generation[NCPU]{};
    eastl::atomic<uint64> g_requested_start[NCPU]{};
    eastl::atomic<uint64> g_requested_size[NCPU]{};
    eastl::atomic<uint32> g_requested_asid[NCPU]{};
    eastl::atomic<proc::ProcessMemoryManager *> g_requested_mm[NCPU]{};
    // 每个 CPU 只访问自己的槽位。非空表示该 mm 的 active 位已经登记且
    // 进入时的遗漏代际已补齐；地址空间切出/exec 时由 leave_mm 清空。
    proc::ProcessMemoryManager *g_current_mm[NCPU]{};
    SpinLock g_mm_flush_lock;
    // 0=未初始化，1=某个 CPU 正在初始化，2=锁已可用。不能在调用
    // SpinLock::init() 之前就发布“已初始化”，否则第二个 CPU 可能直接使用
    // 尚未构造完成的锁。
    eastl::atomic<uint32> g_mm_flush_lock_state{0};

    void ensure_mm_flush_lock()
    {
        if (g_mm_flush_lock_state.load(eastl::memory_order_acquire) == 2)
        {
            return;
        }

        uint32 expected = 0;
        if (g_mm_flush_lock_state.compare_exchange_strong(
                expected, 1, eastl::memory_order_acq_rel))
        {
            g_mm_flush_lock.init("la_mm_tlb_flush");
            g_mm_flush_lock_state.store(2, eastl::memory_order_release);
            return;
        }

        while (g_mm_flush_lock_state.load(eastl::memory_order_acquire) != 2)
        {
            asm volatile("nop");
        }
    }

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

    void flush_local_range_asid(uint32 asid, uint64 start, uint64 size)
    {
        F7LY_PERF_ADD(TlbFlush, 1);
        asm volatile("dbar 0" ::: "memory");
        const auto flush_all_non_global = []()
        {
            F7LY_PERF_ADD(TlbFullFlush, 1);
            // INVTLB op=0x3 失效全部 G=0 表项。范围异常时宁可保守扩大
            // 失效范围，也不能因无符号地址回绕留下可访问的旧翻译。
            asm volatile("invtlb 0x3, $zero, $zero" ::: "memory");
        };
        if (size == 0)
        {
            flush_all_non_global();
            asm volatile("dbar 0" ::: "memory");
            return;
        }

        // LoongArch 一个普通 TLB 表项覆盖相邻两个 4 KiB 页。失效任意
        // 半开区间 [start, start + size) 时，必须先扩张到双页表项边界，
        // 再逐表项处理，保证区间接触到的每个翻译都被失效。
        constexpr uint64 k_tlb_pair_size = PGSIZE << 1;
        constexpr uint64 k_tlb_pair_mask = k_tlb_pair_size - 1;
        constexpr uint64 k_uint64_max = ~static_cast<uint64>(0);
        const uint64 range_end = start + size;
        if (range_end < start || range_end > k_uint64_max - k_tlb_pair_mask)
        {
            flush_all_non_global();
            asm volatile("dbar 0" ::: "memory");
            return;
        }

        const uint64 normalized_start = start & ~k_tlb_pair_mask;
        const uint64 normalized_end =
            (range_end + k_tlb_pair_mask) & ~k_tlb_pair_mask;
        for (uint64 pair_base = normalized_start;
             pair_base < normalized_end;
             pair_base += k_tlb_pair_size)
        {
            asm volatile("invtlb 0x6, %0, %1"
                         :
                         : "r"(static_cast<uint64>(asid)),
                           "r"(pair_base)
                         : "memory");
        }
        asm volatile("dbar 0" ::: "memory");
    }

    // 把需要多个锁/代际变量的冷路径单独保留，避免稳定 usertrapret 命中
    // per-CPU mm 指针时仍保存整套慢路径寄存器。
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
            flush_local_range_asid(mm.user_asid, 0, 0);
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
    g_requested_start[cpu_id].store(0, eastl::memory_order_release);
    g_requested_size[cpu_id].store(0, eastl::memory_order_release);
    g_requested_asid[cpu_id].store(0, eastl::memory_order_release);
    g_requested_mm[cpu_id].store(nullptr, eastl::memory_order_release);
    g_current_mm[cpu_id] = nullptr;
    flush_local_range(0, 0);
    g_acknowledged_generation[cpu_id].store(generation, eastl::memory_order_release);
}

void flush_local_range(uint64 start, uint64 size)
{
    (void)start;
    (void)size;
    F7LY_PERF_ADD(TlbFlush, 1);
    F7LY_PERF_ADD(TlbFullFlush, 1);
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
        proc::ProcessMemoryManager *mm =
            g_requested_mm[cpu_id].load(eastl::memory_order_acquire);
        if (mm != nullptr)
        {
            flush_local_range_asid(
                g_requested_asid[cpu_id].load(eastl::memory_order_acquire),
                g_requested_start[cpu_id].load(eastl::memory_order_acquire),
                g_requested_size[cpu_id].load(eastl::memory_order_acquire));
        }
        else
        {
            flush_local_range(0, 0);
        }
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
    const uint64 ipi_retry_cycles =
        tmm::microseconds_to_cycles(k_ipi_retry_microseconds);
    const uint64 shootdown_timeout_cycles =
        tmm::microseconds_to_cycles(k_shootdown_timeout_microseconds);
    uint64 remote_mask = online_mask;
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        remote_mask &= ~(1ULL << current_cpu);
    }
    F7LY_PERF_ADD(TlbRemoteCpu, perfdiag::count_set_bits(remote_mask));

    // PTE 写入先于 request 的 release；目标 CPU 在 acquire 后失效并回写 ack。
    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((remote_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }
        g_requested_mm[cpu_id].store(nullptr, eastl::memory_order_release);
        g_requested_start[cpu_id].store(0, eastl::memory_order_release);
        g_requested_size[cpu_id].store(0, eastl::memory_order_release);
        g_requested_asid[cpu_id].store(0, eastl::memory_order_release);
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
        uint64 next_retry = wait_start + ipi_retry_cycles;
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
                next_retry = now + ipi_retry_cycles;
            }
            if (now - wait_start >= shootdown_timeout_cycles)
            {
                panic("[tlb] LoongArch shootdown timeout sender=%lu target=%lu generation=%lu ack=%lu",
                      current_cpu, cpu_id, generation,
                      g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire));
            }
        }
    }
}

void enter_mm(proc::ProcessMemoryManager &mm)
{
    const uint64 cpu_id = Cpu::current_cpu_id();
    if (cpu_id >= NCPU)
    {
        return;
    }
    if (g_current_mm[cpu_id] == &mm)
    {
        // active 地址空间的 PTE 更新会等待本 CPU 完成定向 IPI 失效；重复
        // usertrapret 因而无需再次获取 tlb_state_lock 或补做全 ASID invtlb。
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
    ensure_mm_flush_lock();
    mm.tlb_flush_lock.acquire();
    g_mm_flush_lock.acquire();
    asm volatile("dbar 0" ::: "memory");

    const uint64 current_cpu = Cpu::current_cpu_id();
    const uint64 generation = g_generation.fetch_add(1, eastl::memory_order_acq_rel) + 1;
    // 与全核 shootdown 使用同一套时间策略。这里不能继续引用旧的 QEMU
    // 裸周期常量，否则 2K1000 的真实频率下重试与超时长度会完全不同。
    const uint64 ipi_retry_cycles =
        tmm::microseconds_to_cycles(k_ipi_retry_microseconds);
    const uint64 shootdown_timeout_cycles =
        tmm::microseconds_to_cycles(k_shootdown_timeout_microseconds);
    uint64 remote_mask = 0;
    mm.tlb_state_lock.acquire();
    ++mm.tlb_generation;
    remote_mask = mm.tlb_active_cpu_mask;
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        remote_mask &= ~(1ULL << current_cpu);
    }
    mm.tlb_state_lock.release();

    F7LY_PERF_ADD(TlbRemoteCpu, perfdiag::count_set_bits(remote_mask));

    flush_local_range_asid(mm.user_asid, start, size);
    if (Cpu::is_valid_cpu_id(current_cpu))
    {
        mm.tlb_state_lock.acquire();
        if (mm.tlb_seen_generation[current_cpu] < mm.tlb_generation)
        {
            mm.tlb_seen_generation[current_cpu] = mm.tlb_generation;
        }
        mm.tlb_state_lock.release();
    }
    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((remote_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }
        g_requested_mm[cpu_id].store(&mm, eastl::memory_order_release);
        g_requested_asid[cpu_id].store(mm.user_asid, eastl::memory_order_release);
        g_requested_start[cpu_id].store(start, eastl::memory_order_release);
        g_requested_size[cpu_id].store(size, eastl::memory_order_release);
        publish_request(cpu_id, generation);
        send_tlb_ipi(cpu_id);
    }

    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((remote_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }
        const uint64 wait_start = Cpu::get_cpu()->get_time();
        uint64 next_retry = wait_start + ipi_retry_cycles;
        uint32 probe_spins = 0;
        while (g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire) < generation)
        {
            if ((++probe_spins & 0xffU) != 0)
            {
                continue;
            }
            poll_pending();
            const uint64 now = Cpu::get_cpu()->get_time();
            if (static_cast<int64>(now - next_retry) >= 0)
            {
                send_tlb_ipi(cpu_id);
                next_retry = now + ipi_retry_cycles;
            }
            if (now - wait_start >= shootdown_timeout_cycles)
            {
                panic("[tlb] LoongArch mm shootdown timeout sender=%lu target=%lu generation=%lu ack=%lu",
                      current_cpu, cpu_id, generation,
                      g_acknowledged_generation[cpu_id].load(eastl::memory_order_acquire));
            }
        }
    }
    g_mm_flush_lock.release();
    mm.tlb_flush_lock.release();
}

void flush_all_cpus()
{
    flush_range_all_cpus(0, 0);
}

}

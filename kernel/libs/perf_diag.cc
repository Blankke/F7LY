#include "perf_diag.hh"

#if F7LY_PERF_DIAG

#include "hal/cpu.hh"
#include "tm/time.hh"

namespace perfdiag
{
uint64 count_set_bits(uint64 value)
{
    // freestanding 链接不提供 libgcc 的 __popcountdi2。volatile 让诊断构建
    // 保留这个简单移位循环，避免编译器重新识别为需要运行库的 popcount。
    volatile uint64 remaining = value;
    uint64 count = 0;
    while (remaining != 0)
    {
        count += remaining & 1ULL;
        remaining = remaining >> 1;
    }
    return count;
}

    namespace
    {
        constexpr uint64 k_syscall_slots = 512;
        constexpr uint64 k_counter_count = static_cast<uint64>(Counter::Count);

        struct alignas(64) PerCpuCounters
        {
            uint64 values[k_counter_count]{};
            uint64 syscall_count[k_syscall_slots]{};
            uint64 syscall_cycles[k_syscall_slots]{};
        };

        PerCpuCounters g_counters[NUMCPU]{};

        uint64 current_cpu_slot()
        {
            const uint64 cpu = Cpu::current_cpu_id();
            return Cpu::is_valid_cpu_id(cpu) ? cpu : 0;
        }
    }

    uint64 timestamp()
    {
        return tmm::get_hw_time_stamp();
    }

    void add(Counter counter, uint64 value)
    {
        const uint64 index = static_cast<uint64>(counter);
        if (index >= k_counter_count)
        {
            return;
        }
        __atomic_fetch_add(&g_counters[current_cpu_slot()].values[index], value, __ATOMIC_RELAXED);
    }

    void set_max(Counter counter, uint64 value)
    {
        const uint64 index = static_cast<uint64>(counter);
        if (index >= k_counter_count)
        {
            return;
        }
        uint64 *slot = &g_counters[current_cpu_slot()].values[index];
        uint64 previous = __atomic_load_n(slot, __ATOMIC_RELAXED);
        while (previous < value &&
               !__atomic_compare_exchange_n(slot,
                                            &previous,
                                            value,
                                            false,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
        {
        }
    }

    void record_syscall(uint64 number, uint64 elapsed_cycles)
    {
        add(Counter::Syscall);
        add(Counter::SyscallCycles, elapsed_cycles);
        if (number >= k_syscall_slots)
        {
            return;
        }
        PerCpuCounters &cpu = g_counters[current_cpu_slot()];
        __atomic_fetch_add(&cpu.syscall_count[number], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&cpu.syscall_cycles[number], elapsed_cycles, __ATOMIC_RELAXED);
    }

    void reset()
    {
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            for (uint64 index = 0; index < k_counter_count; ++index)
            {
                __atomic_store_n(&g_counters[cpu].values[index], 0, __ATOMIC_RELAXED);
            }
            for (uint64 syscall = 0; syscall < k_syscall_slots; ++syscall)
            {
                __atomic_store_n(&g_counters[cpu].syscall_count[syscall], 0, __ATOMIC_RELAXED);
                __atomic_store_n(&g_counters[cpu].syscall_cycles[syscall], 0, __ATOMIC_RELAXED);
            }
        }
    }

    uint64 counter_sum(Counter counter)
    {
        const uint64 index = static_cast<uint64>(counter);
        uint64 total = 0;
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            total += __atomic_load_n(&g_counters[cpu].values[index], __ATOMIC_RELAXED);
        }
        return total;
    }

    uint64 counter_max(Counter counter)
    {
        const uint64 index = static_cast<uint64>(counter);
        uint64 maximum = 0;
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            const uint64 value = __atomic_load_n(&g_counters[cpu].values[index], __ATOMIC_RELAXED);
            if (value > maximum)
            {
                maximum = value;
            }
        }
        return maximum;
    }

    uint64 syscall_count_sum(uint64 number)
    {
        if (number >= k_syscall_slots)
        {
            return 0;
        }
        uint64 total = 0;
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            total += __atomic_load_n(&g_counters[cpu].syscall_count[number], __ATOMIC_RELAXED);
        }
        return total;
    }

    uint64 syscall_cycles_sum(uint64 number)
    {
        if (number >= k_syscall_slots)
        {
            return 0;
        }
        uint64 total = 0;
        for (uint64 cpu = 0; cpu < NUMCPU; ++cpu)
        {
            total += __atomic_load_n(&g_counters[cpu].syscall_cycles[number], __ATOMIC_RELAXED);
        }
        return total;
    }
}

#endif

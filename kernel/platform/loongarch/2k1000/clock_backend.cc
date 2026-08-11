#include "tm/platform_clock_backend.hh"

#include "libs/printer.hh"

namespace
{
    uint64 g_cached_frequency_hz = 0;

    uint64 detect_frequency_hz()
    {
        // CPUCFG4 给出恒定计数器基频，CPUCFG5 的低/高 16 位分别是乘数和除数。
        // 实机计时必须服从处理器报告值，不能沿用 QEMU 的固定频率。
        uint64 base_hz = 0;
        uint64 ratio = 0;
        uint64 index = 4;
        asm volatile("cpucfg %0, %1" : "=r"(base_hz) : "r"(index));
        index = 5;
        asm volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));

        const uint64 multiplier = ratio & 0xffffU;
        const uint64 divisor = (ratio >> 16) & 0xffffU;
        uint64 detected_hz = 0;

        if (base_hz != 0 && multiplier != 0 && divisor != 0)
        {
            const uint64 quotient = base_hz / divisor;
            const uint64 remainder = base_hz % divisor;
            if (quotient <= ~0ULL / multiplier)
            {
                detected_hz = quotient * multiplier +
                              (remainder * multiplier) / divisor;
            }
        }

        // 错误频率会同时破坏定时中断、设备超时和 POSIX 时钟，因此必须尽早失败。
        constexpr uint64 k_min_valid_frequency_hz = 1'000'000ULL;
        constexpr uint64 k_max_valid_frequency_hz = 10'000'000'000ULL;
        if (detected_hz < k_min_valid_frequency_hz ||
            detected_hz > k_max_valid_frequency_hz)
        {
            panic("[clock] invalid LS2K1000 constant timer frequency: base=%lu ratio=0x%lx",
                  base_hz, ratio);
        }
        return detected_hz;
    }
}

namespace platform::clock_backend
{
    uint64 read_ticks()
    {
        uint64 ticks = 0;
        asm volatile("rdtime.d %0, $zero" : "=r"(ticks));
        return ticks;
    }

    uint64 frequency_hz()
    {
        const uint64 cached =
            __atomic_load_n(&g_cached_frequency_hz, __ATOMIC_ACQUIRE);
        if (cached != 0)
        {
            return cached;
        }

        const uint64 detected = detect_frequency_hz();
        uint64 expected = 0;
        __atomic_compare_exchange_n(&g_cached_frequency_hz, &expected, detected,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        return __atomic_load_n(&g_cached_frequency_hz, __ATOMIC_ACQUIRE);
    }
}

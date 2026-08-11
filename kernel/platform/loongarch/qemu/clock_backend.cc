#include "tm/platform_clock_backend.hh"

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
        // LoongArch QEMU virt 的 rdtime.d 恒定计数器按 100 MHz 工作。
        return 100'000'000ULL;
    }
}

#include "tm/platform_clock_backend.hh"

namespace platform::clock_backend
{
    uint64 read_ticks()
    {
        uint64 ticks = 0;
        asm volatile("rdtime %0" : "=r"(ticks));
        return ticks;
    }

    uint64 frequency_hz()
    {
        // QEMU virt/OpenSBI 向 RISC-V 内核提供 10 MHz 的 time 计数器。
        return 10'000'000ULL;
    }
}

#include "tm/platform_clock_backend.hh"

#include "devs/dtb.hh"
#include "printer.hh"

namespace platform::clock_backend
{
namespace
{
constexpr uint64 k_jh7110_timebase_fallback_hz = 4'000'000ULL;
uint64 g_frequency_hz = 0;
}

uint64 read_ticks()
{
    uint64 ticks = 0;
    asm volatile("rdtime %0" : "=r"(ticks));
    return ticks;
}

uint64 frequency_hz()
{
    if (g_frequency_hz != 0)
    {
        return g_frequency_hz;
    }

    if (DtbManager::get_timebase_frequency(g_frequency_hz))
    {
        platformDiagnosticInfo("[clock] DTB timebase-frequency=%lu Hz\n",
                               g_frequency_hz);
        return g_frequency_hz;
    }

    // StarFive 官方 JH7110 DTS 明确声明 4 MHz。该值只属于当前板级画像，
    // 且 fallback 必须留下可见日志，不能退回 QEMU 的 10 MHz 经验值。
    g_frequency_hz = k_jh7110_timebase_fallback_hz;
    platformDiagnosticWarn(
        "[clock] missing/invalid DTB timebase-frequency, using JH7110 fallback=%lu Hz\n",
        g_frequency_hz);
    return g_frequency_hz;
}
} // namespace platform::clock_backend

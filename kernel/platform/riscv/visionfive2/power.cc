#include "platform/power.hh"

#include "hal/riscv/sbi.hh"

namespace platform::power
{
[[noreturn]] void shutdown()
{
    sbi_shutdown();
    for (;;)
    {
        asm volatile("wfi");
    }
}
} // namespace platform::power

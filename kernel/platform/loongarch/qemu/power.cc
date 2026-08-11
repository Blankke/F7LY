#include "platform/power.hh"

#include "types.hh"

namespace platform::power
{
[[noreturn]] void shutdown()
{
    // QEMU LoongArch virt 的 ACPI poweroff 端口。
    *reinterpret_cast<volatile uint8 *>(0x80000000100e001cULL) = 0x34;
    for (;;)
    {
        asm volatile("idle 0");
    }
}
} // namespace platform::power

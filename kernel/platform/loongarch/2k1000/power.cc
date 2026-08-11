#include "platform/power.hh"

namespace platform::power
{
[[noreturn]] void shutdown()
{
    // 当前板级手册/DTB 没有给出可安全复用的关机控制器，故障路径只停驻 CPU，
    // 不能照搬 QEMU 的 ACPI 魔数去写实机未知 MMIO。
    for (;;)
    {
        asm volatile("idle 0");
    }
}
} // namespace platform::power

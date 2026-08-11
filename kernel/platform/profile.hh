#pragma once

#include "types.hh"

namespace platform
{
enum class Architecture : uint8
{
    Riscv64,
    LoongArch64,
};

enum class Machine : uint8
{
    QemuVirt,
    VisionFive2,
    Loongson2K1000,
};

// 这里只描述当前产物的身份，不承载各驱动的大型函数表。
// 具体能力由 block/net/irq/rtc 等窄接口分别提供。
struct Profile
{
    const char *name;
    Architecture architecture;
    Machine machine;
    bool verbose_boot_diagnostics;
};

// 每个平台目录提供且只提供一个同名实现，链接期保证不会混入两块板。
const Profile &current_profile();
} // namespace platform

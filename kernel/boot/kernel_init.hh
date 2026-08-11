#pragma once

#include "platform/boot_info.hh"

namespace boot
{
// 调用前：全局构造、Console、DTB 和 CPU topology 已就绪，且只有引导核会进入。
[[noreturn]] void initialize_kernel(const platform::BootInfo &boot_info);
} // namespace boot

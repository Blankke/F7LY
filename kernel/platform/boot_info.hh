#pragma once

#include "types.hh"

namespace platform
{
// 架构入口交给公共内核启动流程的全部输入。
// 地址统一是物理地址；入口寄存器/U-Boot argv 等固件细节必须在构造前消化掉。
struct BootInfo
{
    uint64 boot_cpu_hwid;
    uint64 device_tree_paddr;
};
} // namespace platform

#pragma once

#include "types.hh"

namespace loongarch::boot
{
    // 兼容 QEMU/UHI 的直接 DTB 指针，以及 U-Boot `go <entry> <dtb>` 的 argc/argv ABI。
    // 返回物理地址；无法识别或 DTB 头无效时返回 0。
    uint64 resolve_dtb(uint64 arg0, uint64 arg1, uint64 arg2, uint64 arg3);
}

#pragma once

#include "types.hh"

namespace platform
{
// 驱动与页表共享的最小 MMIO 描述。地址始终使用固件/DTB 的物理坐标，
// 是否需要 DMW 别名或页表映射由架构/平台层决定。
struct MmioRegion
{
    uint64 physical_base;
    uint64 size;
};
} // namespace platform

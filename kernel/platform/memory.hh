#pragma once

#include "types.hh"

namespace platform::memory
{
// 把内核可访问地址转换为固件/DTB 使用的物理坐标。RISC-V 恒等映射平台
// 直接返回原值；LoongArch 平台负责去掉 DMW 别名。
uint64 physical_address(uint64 address);

// 把固件给出的物理地址转换为早期内核可以直接解引用的地址。调用方不应
// 自己拼接 DMW/线性映射前缀；传入值即使已经是内核别名也必须可重复转换。
uint64 kernel_access_address(uint64 address);
} // namespace platform::memory

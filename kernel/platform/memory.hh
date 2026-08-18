#pragma once

#include "types.hh"

namespace platform::memory
{
constexpr uint64 k_unlimited_physical_top = ~0ULL;

// 把内核可访问地址转换为固件/DTB 使用的物理坐标。RISC-V 恒等映射平台
// 直接返回原值；LoongArch 平台负责去掉 DMW 别名。
uint64 physical_address(uint64 address);

// 把固件给出的物理地址转换为早期内核可以直接解引用的地址。调用方不应
// 自己拼接 DMW/线性映射前缀；传入值即使已经是内核别名也必须可重复转换。
uint64 kernel_access_address(uint64 address);

// 返回当前平台已经验证可由 PMM/VMM 管理的最高物理地址（左闭右开区间的
// end）；返回 k_unlimited_physical_top 表示只受 DTB RAM 边界约束。
// 该限制描述的是当前内核实现能力，不替代也不扩张 DTB 声明的真实内存。
uint64 managed_physical_top();
} // namespace platform::memory

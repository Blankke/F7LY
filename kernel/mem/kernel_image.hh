#pragma once

#include "types.hh"

// 每份链接脚本都在真实内核起点发布同一个符号。公共代码只能读取这个符号，
// 不能再把 QEMU 的 0x80200000 或某块板的加载地址当成全局事实。
extern "C" char kernel_start[];

namespace mem
{
inline uint64 kernel_image_start_address()
{
    return reinterpret_cast<uint64>(kernel_start);
}
} // namespace mem

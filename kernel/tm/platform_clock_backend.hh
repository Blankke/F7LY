#pragma once

#include "types.hh"

namespace platform::clock_backend
{
    // 读取当前平台恒定计数器。返回值必须与 frequency_hz() 使用同一计数域，
    // 通用时间层不应为了读时钟反向依赖进程管理器或具体架构 CSR。
    uint64 read_ticks();

    /**
     * @brief 返回当前平台恒定计数器的频率（Hz）。
     *
     * 时间换算只依赖这一项能力。平台实现必须返回稳定、非零的频率；
     * 无法可靠确定频率时应立即 panic，不能使用其他机器的经验值兜底。
     */
    uint64 frequency_hz();
}

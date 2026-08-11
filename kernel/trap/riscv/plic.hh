#pragma once

#include "types.hh"

namespace riscv::plic
{
    // PLIC 只负责寄存器事务。设备处理函数由 hal::irq 公共层管理。
    void initialize_global();
    void initialize_current_cpu(uint64 enabled_sources);
    bool enable_source(uint32 source);
    uint32 claim();
    void complete(uint32 source);
} // namespace riscv::plic

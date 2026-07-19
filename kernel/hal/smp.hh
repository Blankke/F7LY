#pragma once

#include "types.hh"

namespace hal::smp
{
    // 所有 CPU 进入 C++ main() 后最先经过此入口。主核认领启动权并返回；
    // 次核在架构 HAL 内完成本地初始化、进入调度器，此函数不再返回。
    void enter(uint64 cpu_id, uint64 boot_argument);

    // DTB 已可访问后由主核调用，发布 possible CPU 拓扑。
    void configure_topology();

    // 全局子系统和 scheduler 已初始化后由主核调用。该函数启动所有次核，
    // 等待它们完成本地初始化，再统一放行调度器。
    void start_secondaries(uint64 boot_argument);

    // 停止不属于 possible 集合的 CPU，或承接调度器意外返回后的兜底路径。
    [[noreturn]] void park_current_cpu();
}

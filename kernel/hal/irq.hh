#pragma once

#include "types.hh"

namespace hal::irq
{
    using Source = uint32;
    using Handler = void (*)(void *context);

    // 公共分发层使用一个 64 位 pending 集合，因而硬件源号的统一范围是 0..63。
    // 这里的 source 是中断控制器内部 hwirq，不是 CPU 异常号或 Linux virq。
    inline constexpr Source k_max_sources = 64;

    /**
     * 注册设备中断处理函数。同一 source 最多可挂四个处理函数，以容纳 PCI
     * 共享中断；重复注册完全相同的 handler/context 会被视为成功。
     *
     * 该接口不分配内存，也不依赖 C++ 全局构造。设备可以在全局控制器初始化
     * 前登记，控制器初始化时会一次性启用已经登记的 source。
     */
    bool register_handler(Source source, Handler handler, void *context,
                          const char *name);

    // 全局控制器状态只由启动核初始化一次；每个 CPU 还要初始化自己的 context。
    void initialize_global();
    void initialize_current_cpu();

    // 从控制器领取一次事务，分发其中全部 source，并用原始令牌完成同一事务。
    // trap 只应调用此入口，不应再识别任何具体设备。
    void dispatch();

    // 主要用于启动诊断和控制器本地 context 初始化。
    uint64 registered_sources();
} // namespace hal::irq

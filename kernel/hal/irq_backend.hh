#pragma once

#include "hal/irq.hh"

namespace hal::irq
{
    /**
     * claim 与 complete 之间不可拆分的控制器事务。
     *
     * PLIC 每次只返回一个 IRQ：pending_sources 是 1 << IRQ，controller_token
     * 保存原始 IRQ。LoongArch 控制器一次返回位图，两者都保存同一位图。
     * complete 必须收到原始 controller_token，不能从处理后的 pending 反推。
     */
    struct ClaimToken
    {
        uint64 pending_sources;
        uint64 controller_token;
    };

    namespace backend
    {
        // 每个平台目录恰好提供一份同名实现；这里只允许控制器寄存器事务。
        bool supports_source(Source source);
        void initialize_global();
        void initialize_current_cpu(uint64 registered_sources);
        void enable_source(Source source);
        ClaimToken claim();
        void complete(const ClaimToken &token);
    } // namespace backend
} // namespace hal::irq

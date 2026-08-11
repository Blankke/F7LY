#include "hal/irq_backend.hh"

#include "hal/loongarch/platform_board.hh"
#include "ls2k1000_liointc.hh"

namespace hal::irq::backend
{
bool supports_source(Source source)
{
    return source < loongarch::board::k_liointc_input_count;
}

void initialize_global()
{
    // 路由和触发类型先建立，但在公共 handler 表逐项登记前不开放任何输入。
    loongarch::liointc::init(0);
}

void initialize_current_cpu(uint64 registered_sources)
{
    (void)registered_sources;
    // 2K1000 LIOINTC 当前固定级联到启动核 HWI1，没有每核 context。
}

void enable_source(Source source)
{
    (void)loongarch::liointc::enable(source);
}

ClaimToken claim()
{
    const uint64 pending = loongarch::liointc::claim();
    return ClaimToken{
        .pending_sources = pending,
        .controller_token = pending,
    };
}

void complete(const ClaimToken &token)
{
    loongarch::liointc::complete(static_cast<uint32>(token.controller_token));
}
} // namespace hal::irq::backend

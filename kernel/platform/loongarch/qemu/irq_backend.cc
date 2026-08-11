#include "hal/irq_backend.hh"

#include "apic.hh"
#include "extioi.hh"

namespace hal::irq::backend
{
bool supports_source(Source source)
{
    return source < k_max_sources;
}

void initialize_global()
{
    apic_init();
    extioi_init();
}

void initialize_current_cpu(uint64 registered_sources)
{
    (void)registered_sources;
    // QEMU 当前把 PCH/ExtIOI 外部中断统一路由到启动核，没有每核 context。
}

void enable_source(Source source)
{
    (void)apic_enable(source);
    (void)extioi_enable(source);
}

ClaimToken claim()
{
    const uint64 pending = extioi_claim();
    return ClaimToken{
        .pending_sources = pending,
        .controller_token = pending,
    };
}

void complete(const ClaimToken &token)
{
    apic_complete(token.controller_token);
    extioi_complete(token.controller_token);
}
} // namespace hal::irq::backend

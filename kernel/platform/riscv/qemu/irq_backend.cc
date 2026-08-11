#include "hal/irq_backend.hh"

#include "trap/riscv/plic.hh"

namespace hal::irq::backend
{
bool supports_source(Source source)
{
    // PLIC source 0 表示“本次没有可领取的中断”。
    return source > 0 && source < k_max_sources;
}

void initialize_global()
{
    riscv::plic::initialize_global();
}

void initialize_current_cpu(uint64 registered_sources)
{
    riscv::plic::initialize_current_cpu(registered_sources);
}

void enable_source(Source source)
{
    (void)riscv::plic::enable_source(source);
}

ClaimToken claim()
{
    const uint32 source = riscv::plic::claim();
    return ClaimToken{
        .pending_sources = source > 0 && source < k_max_sources
                               ? (1ULL << source)
                               : 0,
        .controller_token = source,
    };
}

void complete(const ClaimToken &token)
{
    riscv::plic::complete(static_cast<uint32>(token.controller_token));
}
} // namespace hal::irq::backend

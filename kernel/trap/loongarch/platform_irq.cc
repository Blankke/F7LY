#include "platform_irq.hh"

#include "hal/loongarch/platform_board.hh"

#ifdef BOARD_LS2K1000
#include "ls2k1000_liointc.hh"
#else
#include "apic.hh"
#include "extioi.hh"
#endif

namespace loongarch::platform_irq
{
uint64 uart_source_mask()
{
    return 1ULL << board::k_uart_interrupt;
}

uint64 block_source_mask()
{
#ifdef BOARD_LS2K1000
    return 1ULL << board::k_ahci_interrupt;
#else
    return 1ULL << board::k_virtio_block_interrupt;
#endif
}

void init()
{
#ifdef BOARD_LS2K1000
    // 当前 AHCI 使用同步轮询完成，不能提前开放未注册处理函数的 IRQ。
    liointc::init(static_cast<uint32>(uart_source_mask()));
#else
    apic_init();
    extioi_init();
#endif
}

uint64 claim()
{
#ifdef BOARD_LS2K1000
    return liointc::claim();
#else
    return extioi_claim();
#endif
}

void complete(uint64 sources)
{
#ifdef BOARD_LS2K1000
    liointc::complete(static_cast<uint32>(sources));
#else
    apic_complete(sources);
    extioi_complete(sources);
#endif
}
} // namespace loongarch::platform_irq

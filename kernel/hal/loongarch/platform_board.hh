#pragma once

#include "types.hh"

namespace loongarch::board
{
    constexpr uint64 k_physical_address_mask = 0x0000ffffffffffffULL;
    constexpr uint64 k_uncached_dmw_base = 0x8000000000000000ULL;
    constexpr uint64 k_cached_dmw_base = 0x9000000000000000ULL;

    constexpr uint64 physical_address(uint64 address)
    {
        return address & k_physical_address_mask;
    }

    constexpr uint64 cached_address(uint64 physical)
    {
        return physical_address(physical) | k_cached_dmw_base;
    }

    constexpr uint64 mmio_address(uint64 physical)
    {
        return physical_address(physical) | k_uncached_dmw_base;
    }

#ifdef BOARD_LS2K1000
    constexpr const char *k_name = "Loongson 2K1000";
    constexpr uint64 k_uart_physical = 0x1fe20000ULL;
    constexpr uint32 k_uart_interrupt = 0;

    constexpr uint64 k_liointc_registers_physical = 0x1fe01400ULL;
    constexpr uint64 k_liointc_isr_physical = 0x1fe01040ULL;
    constexpr uint32 k_liointc_input_count = 32;
    // JL-LSGD2K10 DTB 把 LIOINTC 级联到 CPU HWI1，即架构中断号 3。
    constexpr uint32 k_external_cpu_interrupt = 3;

    constexpr uint64 k_ahci_physical = 0x400e0000ULL;
    constexpr uint32 k_ahci_interrupt = 19;
    constexpr uint32 k_ahci_port_fallback = 1;
    constexpr bool k_has_virtio_block = false;
    constexpr bool k_has_virtio_network = false;
    constexpr uint64 k_gmac0_physical = 0x40040000ULL;
    constexpr uint32 k_gmac0_interrupt = 12;
    constexpr uint64 k_rtc_physical = 0x1fe27800ULL;
#else
    constexpr const char *k_name = "QEMU LoongArch virt";
    constexpr uint64 k_uart_physical = 0x1fe001e0ULL;
    constexpr uint32 k_uart_interrupt = 2;
    constexpr uint32 k_external_cpu_interrupt = 3;
    constexpr uint32 k_virtio_block_interrupt = 32;
    constexpr bool k_has_virtio_block = true;
    constexpr bool k_has_virtio_network = true;
#endif

    constexpr uint64 k_uart_mmio = mmio_address(k_uart_physical);
} // namespace loongarch::board

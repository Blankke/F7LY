#pragma once

#include "types.hh"
#include "platform_board_config.hh"

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

    constexpr uint64 k_uart_mmio = mmio_address(k_uart_physical);
} // namespace loongarch::board

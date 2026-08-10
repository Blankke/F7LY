#pragma once

#include "types.hh"

namespace net::vf2
{
    constexpr uint32 k_ring_count = 32;
    constexpr uint32 k_rx_buffer_size = 2048;
    constexpr uint32 k_max_frame_size = 1536;
    constexpr uint32 k_mdio_timeout = 100000;
    // Match Linux dwmac4_dma_reset's one-second polling window.
    constexpr uint32 k_dma_reset_timeout = 1000000;

    uint32 read_reg(uint64 base, uint32 offset);
    void write_reg(uint64 base, uint32 offset, uint32 value);
    void delay_us(uint32 usec);
    void io_fence();
    uint64 dma_address(const void *ptr);
    bool platform_init();
    void platform_set_speed(uint32 speed_mbps);
}

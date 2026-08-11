/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "types.hh"

// Synopsys DWMAC 5.20 / GMAC4 寄存器与普通 16-byte descriptor 定义。
// 位定义依据 Linux stmmac 的 dwmac4_dma.h、dwmac4_descs.h；最初的
// VisionFive 2 bring-up 来自 F7LY/RocketOS，现只保留经上游资料核对的部分。
namespace riscv::jh7110::gmac::regs
{
// MAC core
inline constexpr uint32 k_mac_configuration = 0x0000;
inline constexpr uint32 k_mac_packet_filter = 0x0008;
inline constexpr uint32 k_mac_rxq_ctrl0 = 0x00a0;
inline constexpr uint32 k_mac_mdio_address = 0x0200;
inline constexpr uint32 k_mac_mdio_data = 0x0204;
inline constexpr uint32 k_mac_address0_high = 0x0300;
inline constexpr uint32 k_mac_address0_low = 0x0304;

inline constexpr uint32 k_mac_receive_enable = 1U << 0;
inline constexpr uint32 k_mac_transmit_enable = 1U << 1;
inline constexpr uint32 k_mac_duplex = 1U << 13;
inline constexpr uint32 k_mac_fast_ethernet_speed = 1U << 14;
inline constexpr uint32 k_mac_port_select = 1U << 15;
inline constexpr uint32 k_mac_address_enable = 1U << 31;
inline constexpr uint32 k_mac_rxq0_enable_dcb = 2U;

// MTL queue 0
inline constexpr uint32 k_mtl_txq0_operation_mode = 0x0d00;
inline constexpr uint32 k_mtl_rxq0_operation_mode = 0x0d30;
inline constexpr uint32 k_mtl_tx_store_and_forward = 1U << 1;
inline constexpr uint32 k_mtl_tx_queue_enable = 2U << 2;
inline constexpr uint32 k_mtl_tx_queue_size_2048 = 7U << 16;
inline constexpr uint32 k_mtl_rx_store_and_forward = 1U << 5;
inline constexpr uint32 k_mtl_rx_queue_size_2048 = 7U << 20;

// DMA common/channel 0
inline constexpr uint32 k_dma_mode = 0x1000;
inline constexpr uint32 k_dma_sysbus_mode = 0x1004;
inline constexpr uint32 k_dma_channel0_control = 0x1100;
inline constexpr uint32 k_dma_channel0_tx_control = 0x1104;
inline constexpr uint32 k_dma_channel0_rx_control = 0x1108;
inline constexpr uint32 k_dma_channel0_tx_desc_list_high = 0x1110;
inline constexpr uint32 k_dma_channel0_tx_desc_list = 0x1114;
inline constexpr uint32 k_dma_channel0_rx_desc_list_high = 0x1118;
inline constexpr uint32 k_dma_channel0_rx_desc_list = 0x111c;
inline constexpr uint32 k_dma_channel0_tx_tail = 0x1120;
inline constexpr uint32 k_dma_channel0_rx_tail = 0x1128;
inline constexpr uint32 k_dma_channel0_tx_ring_length = 0x112c;
inline constexpr uint32 k_dma_channel0_rx_ring_length = 0x1130;
inline constexpr uint32 k_dma_channel0_interrupt_enable = 0x1134;
inline constexpr uint32 k_dma_channel0_status = 0x1160;

inline constexpr uint32 k_dma_software_reset = 1U << 0;
inline constexpr uint32 k_dma_descriptor_cache_enable = 1U << 19;
inline constexpr uint32 k_dma_sysbus_fixed_burst = 1U << 0;
inline constexpr uint32 k_dma_sysbus_blen32 = 1U << 4;
inline constexpr uint32 k_dma_sysbus_blen64 = 1U << 5;
inline constexpr uint32 k_dma_sysbus_blen128 = 1U << 6;
inline constexpr uint32 k_dma_sysbus_blen256 = 1U << 7;
inline constexpr uint32 k_dma_sysbus_enhanced_addressing = 1U << 11;
inline constexpr uint32 k_dma_sysbus_address_aligned_beats = 1U << 12;
inline constexpr uint32 k_dma_sysbus_read_osr_15 = 15U << 16;
inline constexpr uint32 k_dma_sysbus_write_osr_15 = 15U << 24;
inline constexpr uint32 k_dma_channel_start = 1U << 0;
inline constexpr uint32 k_dma_tx_operate_on_second_packet = 1U << 4;
inline constexpr uint32 k_dma_tx_pbl_16 = 16U << 16;
inline constexpr uint32 k_dma_rx_buffer_size_shift = 1;
inline constexpr uint32 k_dma_rx_pbl_16 = 16U << 16;
inline constexpr uint32 k_dma_status_context_descriptor_error = 1U << 13;
inline constexpr uint32 k_dma_status_fatal_bus_error = 1U << 12;

// MDIO。JH7110 GMAC CSR 时钟位于 250--300 MHz 档，CR=5 对应 /122。
inline constexpr uint32 k_mdio_busy = 1U << 0;
inline constexpr uint32 k_mdio_goc_shift = 2;
inline constexpr uint32 k_mdio_goc_write = 1U;
inline constexpr uint32 k_mdio_goc_read = 3U;
inline constexpr uint32 k_mdio_clock_range_shift = 8;
inline constexpr uint32 k_mdio_clock_range_250_300 = 5U;
inline constexpr uint32 k_mdio_register_shift = 16;
inline constexpr uint32 k_mdio_phy_shift = 21;

// 普通 TX descriptor：IOC 位于 DES2[31]，不是旧实现使用的 bit30。
inline constexpr uint32 k_tx_buffer1_size_mask = 0x3fffU;
inline constexpr uint32 k_tx_interrupt_on_completion = 1U << 31;
inline constexpr uint32 k_tx_packet_size_mask = 0x7fffU;
inline constexpr uint32 k_tx_error_summary = 1U << 15;
inline constexpr uint32 k_tx_last_descriptor = 1U << 28;
inline constexpr uint32 k_tx_first_descriptor = 1U << 29;
inline constexpr uint32 k_tx_owned_by_dma = 1U << 31;

// 普通 RX descriptor read/write-back 格式。
inline constexpr uint32 k_rx_packet_size_mask = 0x7fffU;
inline constexpr uint32 k_rx_error_summary = 1U << 15;
inline constexpr uint32 k_rx_dribble_error = 1U << 19;
inline constexpr uint32 k_rx_receive_error = 1U << 20;
inline constexpr uint32 k_rx_overflow_error = 1U << 21;
inline constexpr uint32 k_rx_watchdog_error = 1U << 22;
inline constexpr uint32 k_rx_giant_packet = 1U << 23;
inline constexpr uint32 k_rx_crc_error = 1U << 24;
inline constexpr uint32 k_rx_last_descriptor = 1U << 28;
inline constexpr uint32 k_rx_first_descriptor = 1U << 29;
inline constexpr uint32 k_rx_context_descriptor = 1U << 30;
inline constexpr uint32 k_rx_buffer1_valid = 1U << 24;
inline constexpr uint32 k_rx_interrupt_on_completion = 1U << 30;
inline constexpr uint32 k_rx_owned_by_dma = 1U << 31;
inline constexpr uint32 k_rx_hardware_error_mask =
    k_rx_error_summary | k_rx_dribble_error | k_rx_receive_error |
    k_rx_overflow_error | k_rx_watchdog_error | k_rx_giant_packet |
    k_rx_crc_error;

// Clause 22 PHY / Motorcomm YT8531
inline constexpr uint16 k_phy_bmcr = 0x00;
inline constexpr uint16 k_phy_id1 = 0x02;
inline constexpr uint16 k_phy_id2 = 0x03;
inline constexpr uint16 k_phy_advertise = 0x04;
inline constexpr uint16 k_phy_ctrl1000 = 0x09;
inline constexpr uint16 k_phy_specific_status = 0x11;
inline constexpr uint16 k_phy_extended_address = 0x1e;
inline constexpr uint16 k_phy_extended_data = 0x1f;

inline constexpr uint16 k_bmcr_reset = 1U << 15;
inline constexpr uint16 k_bmcr_autoneg_enable = 1U << 12;
inline constexpr uint16 k_bmcr_autoneg_restart = 1U << 9;
inline constexpr uint16 k_phy_advertise_default = 0x01e1;
inline constexpr uint16 k_phy_advertise_1000 = 0x0300;

inline constexpr uint32 k_yt8531_id = 0x4f51e91bU;
inline constexpr uint16 k_yt_status_speed_mask = 3U << 14;
inline constexpr uint16 k_yt_status_speed_100 = 1U << 14;
inline constexpr uint16 k_yt_status_speed_1000 = 2U << 14;
inline constexpr uint16 k_yt_status_duplex = 1U << 13;
inline constexpr uint16 k_yt_status_resolved = 1U << 11;
inline constexpr uint16 k_yt_status_link = 1U << 10;

inline constexpr uint16 k_yt_chip_config = 0xa001;
inline constexpr uint16 k_yt_rgmii_config = 0xa003;
inline constexpr uint16 k_yt_rxc_delay_enable = 1U << 8;
inline constexpr uint16 k_yt_rx_delay_mask = 0x0fU << 10;
inline constexpr uint16 k_yt_ge_tx_delay_mask = 0x0fU;
inline constexpr uint16 k_yt_delay_1_95_ns = 13U;

struct DmaDescriptor
{
    uint32 des0;
    uint32 des1;
    uint32 des2;
    uint32 des3;
};

static_assert(sizeof(DmaDescriptor) == 16,
              "DWMAC4 normal descriptor must be exactly 16 bytes");
} // namespace riscv::jh7110::gmac::regs

#pragma once

#include "types.hh"

namespace net::vf2
{
    constexpr uint32 k_mac_configuration = 0x0000;
    constexpr uint32 k_mac_packet_filter = 0x0008;
    constexpr uint32 k_mac_address_high = 0x0300;
    constexpr uint32 k_mac_address_low = 0x0304;
    constexpr uint32 k_mac_mdio_address = 0x0200;
    constexpr uint32 k_mac_mdio_data = 0x0204;
    constexpr uint32 k_mac_rxq_ctrl0 = 0x00a0;

    constexpr uint32 k_mtl_rxq0_operation_mode = 0x0d30;
    constexpr uint32 k_mtl_txq0_operation_mode = 0x0d00;

    constexpr uint32 k_dma_mode = 0x1000;
    constexpr uint32 k_dma_sysbus_mode = 0x1004;
    constexpr uint32 k_dma_ch0_tx_control = 0x1104;
    constexpr uint32 k_dma_ch0_rx_control = 0x1108;
    constexpr uint32 k_dma_ch0_tx_desc_list = 0x1114;
    constexpr uint32 k_dma_ch0_tx_desc_list_high = 0x1110;
    constexpr uint32 k_dma_ch0_rx_desc_list = 0x111c;
    constexpr uint32 k_dma_ch0_rx_desc_list_high = 0x1118;
    constexpr uint32 k_dma_ch0_tx_tail = 0x1120;
    constexpr uint32 k_dma_ch0_rx_tail = 0x1128;
    constexpr uint32 k_dma_ch0_tx_ring_len = 0x112c;
    constexpr uint32 k_dma_ch0_rx_ring_len = 0x1130;
    constexpr uint32 k_dma_ch0_interrupt_enable = 0x1134;
    constexpr uint32 k_dma_ch0_status = 0x1160;

    constexpr uint32 k_mac_re = 1u << 0;
    constexpr uint32 k_mac_te = 1u << 1;
    constexpr uint32 k_mac_dm = 1u << 13;
    constexpr uint32 k_mac_fes = 1u << 14;
    constexpr uint32 k_mac_ps = 1u << 15;

    constexpr uint32 k_dma_swr = 1u << 0;
    constexpr uint32 k_dma_sysbus_fb = 1u << 0;
    constexpr uint32 k_dma_sysbus_aal = 1u << 12;
    constexpr uint32 k_dma_start = 1u << 0;
    constexpr uint32 k_dma_rx_buf_size_shift = 1;

    constexpr uint32 k_desc_own = 1u << 31;
    constexpr uint32 k_desc_fd = 1u << 29;
    constexpr uint32 k_desc_ld = 1u << 28;
    constexpr uint32 k_desc_ioc = 1u << 30;
    constexpr uint32 k_desc_buf1_valid = 1u << 24;
    constexpr uint32 k_desc_len_mask = 0x7fffu;
    // DWMAC 5.20 uses different descriptor words for TX and RX IOC.
    constexpr uint32 k_tx_desc_ioc = 1u << 31;
    constexpr uint32 k_tx_desc_error_summary = 1u << 15;

    constexpr uint32 k_mdio_busy = 1u << 0;
    constexpr uint32 k_mdio_goc_shift = 2;
    constexpr uint32 k_mdio_goc_read = 3u;
    constexpr uint32 k_mdio_goc_write = 1u;
    constexpr uint32 k_mdio_cr_shift = 8;
    // JH7110 GMAC CSR clock is in the 250-300 MHz range; RocketOS uses CR=5.
    constexpr uint32 k_mdio_cr_250_300 = 5u;
    constexpr uint32 k_mdio_rda_shift = 16;
    constexpr uint32 k_mdio_pa_shift = 21;

    constexpr uint16 k_phy_bmcr = 0;
    constexpr uint16 k_phy_bmsr = 1;
    constexpr uint16 k_phy_id1 = 2;
    constexpr uint16 k_phy_id2 = 3;
    constexpr uint16 k_phy_advertise = 4;
    constexpr uint16 k_phy_ctrl1000 = 9;
    constexpr uint16 k_phy_spec_status = 0x11;

    constexpr uint16 k_bmcr_reset = 1u << 15;
    constexpr uint16 k_bmcr_an_enable = 1u << 12;
    constexpr uint16 k_bmcr_an_restart = 1u << 9;
    constexpr uint16 k_bmsr_link = 1u << 2;

    constexpr uint32 k_yt8531_id = 0x4f51e91b;
    constexpr uint32 k_yt_speed_mask = 0xc000;
    constexpr uint32 k_yt_speed_shift = 14;
    constexpr uint32 k_yt_duplex = 1u << 13;
    constexpr uint16 k_yt_ext_address = 0x1e;
    constexpr uint16 k_yt_ext_data = 0x1f;
    constexpr uint16 k_yt_ext_chip_config = 0xa001;
    constexpr uint16 k_yt_ext_rgmii_config = 0xa003;
    constexpr uint16 k_yt_rxc_delay_enable = 1u << 8;
    constexpr uint16 k_yt_rx_delay_mask = 0x0fu << 10;
    constexpr uint16 k_yt_ge_tx_delay_mask = 0x0fu;
    constexpr uint16 k_yt_delay_1_95ns = 13u;

    struct DmaDescriptor
    {
        uint32 des0;
        uint32 des1;
        uint32 des2;
        uint32 des3;
    } __attribute__((packed));
}

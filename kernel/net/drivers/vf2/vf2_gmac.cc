/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Based on the RocketOS VisionFive2 driver, revision
 * 1e1fc238f6ffa17b262163f029d48f660943cd96.
 * Ported to F7LY C++ and adapted to the F7LY DMA/ONPS interfaces.
 */
#include "vf2_gmac.hh"

#include "devs/spinlock.hh"
#include "libs/klib.hh"
#include "libs/printer.hh"
#include "mem/memlayout.hh"
#include "virtual_memory_manager.hh"
#include "vf2_gmac_platform.hh"
#include "vf2_gmac_regs.hh"

namespace net
{
    namespace
    {
        using namespace vf2;

        struct Vf2GmacState
        {
            bool initialized = false;
            bool link_up = false;
            bool link_reported = false;
            bool phy_link_up = false;
            bool full_duplex = true;
            uint16 phy_addr = 0;
            uint32 speed = 1000;
            uint32 tx_head = 0;
            uint32 tx_tail = 0;
            uint32 rx_head = 0;
            uint64 tx_packets = 0;
            uint64 tx_completed = 0;
            uint64 rx_packets = 0;
            uint64 tx_errors = 0;
            uint64 rx_errors = 0;
            uint8 mac[6] = {0};
            SpinLock lock;
        };

        static Vf2GmacState g_state;
#ifdef VISIONFIVE2
        static DmaDescriptor g_tx_ring[k_ring_count] __attribute__((aligned(64)));
        static DmaDescriptor g_rx_ring[k_ring_count] __attribute__((aligned(64)));
        static uint8 g_tx_buffers[k_ring_count][k_rx_buffer_size] __attribute__((aligned(64)));
        static uint8 g_rx_buffers[k_ring_count][k_rx_buffer_size] __attribute__((aligned(64)));
#endif

#ifdef VISIONFIVE2
        constexpr uint64 k_base = VF2_GMAC1_BASE_V;

        inline uint32 mac_read(uint32 offset)
        {
            return vf2::read_reg(k_base, offset);
        }

        inline void mac_write(uint32 offset, uint32 value)
        {
            vf2::write_reg(k_base, offset, value);
        }

        bool wait_dma_reset()
        {
            for (uint32 i = 0; i < k_dma_reset_timeout; ++i)
            {
                if ((mac_read(k_dma_mode) & k_dma_swr) == 0)
                    return true;
                vf2::delay_us(1);
            }
            return false;
        }

        bool mdio_wait_idle()
        {
            for (uint32 i = 0; i < k_mdio_timeout; ++i)
            {
                if ((mac_read(k_mac_mdio_address) & k_mdio_busy) == 0)
                    return true;
                vf2::delay_us(1);
            }
            return false;
        }

        int mdio_read(uint16 phy, uint16 reg, uint16 *value)
        {
            if (value == nullptr || phy > 31 || reg > 31 || !mdio_wait_idle())
                return -1;

            uint32 address = (static_cast<uint32>(phy) << k_mdio_pa_shift) |
                             (static_cast<uint32>(reg) << k_mdio_rda_shift) |
                             (k_mdio_cr_250_300 << k_mdio_cr_shift) |
                             (k_mdio_goc_read << k_mdio_goc_shift) | k_mdio_busy;
            mac_write(k_mac_mdio_address, address);
            if (!mdio_wait_idle())
                return -1;
            *value = static_cast<uint16>(mac_read(k_mac_mdio_data) & 0xffffu);
            return 0;
        }

        int mdio_write(uint16 phy, uint16 reg, uint16 value)
        {
            if (phy > 31 || reg > 31 || !mdio_wait_idle())
                return -1;

            mac_write(k_mac_mdio_data, value);
            uint32 address = (static_cast<uint32>(phy) << k_mdio_pa_shift) |
                             (static_cast<uint32>(reg) << k_mdio_rda_shift) |
                             (k_mdio_cr_250_300 << k_mdio_cr_shift) |
                             (k_mdio_goc_write << k_mdio_goc_shift) | k_mdio_busy;
            mac_write(k_mac_mdio_address, address);
            return mdio_wait_idle() ? 0 : -1;
        }

        int yt_ext_read(uint16 phy, uint16 reg, uint16 *value)
        {
            if (mdio_write(phy, k_yt_ext_address, reg) < 0)
                return -1;
            return mdio_read(phy, k_yt_ext_data, value);
        }

        int yt_ext_write(uint16 phy, uint16 reg, uint16 value)
        {
            if (mdio_write(phy, k_yt_ext_address, reg) < 0)
                return -1;
            return mdio_write(phy, k_yt_ext_data, value);
        }

        bool valid_mac(const uint8 mac[6])
        {
            bool all_zero = true;
            bool all_ff = true;
            for (int i = 0; i < 6; ++i)
            {
                all_zero = all_zero && mac[i] == 0;
                all_ff = all_ff && mac[i] == 0xff;
            }
            return !all_zero && !all_ff && (mac[0] & 1) == 0;
        }

        void read_or_set_mac()
        {
            uint32 low = mac_read(k_mac_address_low);
            uint32 high = mac_read(k_mac_address_high);
            g_state.mac[0] = static_cast<uint8>(low);
            g_state.mac[1] = static_cast<uint8>(low >> 8);
            g_state.mac[2] = static_cast<uint8>(low >> 16);
            g_state.mac[3] = static_cast<uint8>(low >> 24);
            g_state.mac[4] = static_cast<uint8>(high);
            g_state.mac[5] = static_cast<uint8>(high >> 8);

            if (!valid_mac(g_state.mac))
            {
                const uint8 fallback[6] = {0x02, 0xf7, 0x4c, 0x59, 0x02, 0x01};
                memcpy(g_state.mac, fallback, sizeof(fallback));
            }
        }

        bool init_phy()
        {
            uint16 id1 = 0;
            uint16 id2 = 0;
            g_state.phy_addr = 0xffff;
            for (uint16 phy = 0; phy < 32; ++phy)
            {
                if (mdio_read(phy, k_phy_id1, &id1) < 0 ||
                    mdio_read(phy, k_phy_id2, &id2) < 0)
                    continue;
                if (id1 == 0 || id1 == 0xffff || id2 == 0 || id2 == 0xffff)
                    continue;
                g_state.phy_addr = phy;
                break;
            }
            if (g_state.phy_addr == 0xffff)
                return false;

            uint32 phy_id = (static_cast<uint32>(id1) << 16) | id2;
            printf("[vf2_gmac] PHY addr=%u id=0x%x%s\n", g_state.phy_addr, phy_id,
                   phy_id == k_yt8531_id ? " (YT8531)" : "");

            if (mdio_write(g_state.phy_addr, k_phy_bmcr, k_bmcr_reset) < 0)
                return false;
            for (uint32 i = 0; i < 500; ++i)
            {
                uint16 bmcr = 0;
                if (mdio_read(g_state.phy_addr, k_phy_bmcr, &bmcr) < 0)
                    return false;
                if ((bmcr & k_bmcr_reset) == 0)
                    break;
                vf2::delay_us(1000);
            }

            // The board DTS uses rgmii-id. Match the Linux-verified YT8531
            // setup: clear the additional RXC delay and apply 1.95 ns delays
            // through the regular RX/TX delay fields.
            uint16 chip_config = 0;
            uint16 rgmii_config = 0;
            if (yt_ext_read(g_state.phy_addr, k_yt_ext_chip_config, &chip_config) < 0 ||
                yt_ext_read(g_state.phy_addr, k_yt_ext_rgmii_config, &rgmii_config) < 0)
                return false;

            uint16 new_chip_config = static_cast<uint16>(
                chip_config & ~k_yt_rxc_delay_enable);
            uint16 new_rgmii_config = static_cast<uint16>(
                (rgmii_config & ~(k_yt_rx_delay_mask | k_yt_ge_tx_delay_mask)) |
                (k_yt_delay_1_95ns << 10) | k_yt_delay_1_95ns);
            if (yt_ext_write(g_state.phy_addr, k_yt_ext_chip_config, new_chip_config) < 0 ||
                yt_ext_write(g_state.phy_addr, k_yt_ext_rgmii_config, new_rgmii_config) < 0 ||
                yt_ext_read(g_state.phy_addr, k_yt_ext_chip_config, &chip_config) < 0 ||
                yt_ext_read(g_state.phy_addr, k_yt_ext_rgmii_config, &rgmii_config) < 0)
                return false;

            printf("[vf2_gmac] YT8531 rgmii-id a001=0x%x a003=0x%x\n",
                   chip_config, rgmii_config);

            const uint16 advertise = 0x01e1;
            if (mdio_write(g_state.phy_addr, k_phy_advertise, advertise) < 0 ||
                mdio_write(g_state.phy_addr, k_phy_ctrl1000, 0x0300) < 0 ||
                mdio_write(g_state.phy_addr, k_phy_bmcr,
                           k_bmcr_an_enable | k_bmcr_an_restart) < 0)
                return false;
            return true;
        }

        void update_link_state()
        {
            if (g_state.phy_addr == 0xffff)
                return;
            uint16 bmsr = 0;
            uint16 status = 0;
            if (mdio_read(g_state.phy_addr, k_phy_bmsr, &bmsr) < 0 ||
                mdio_read(g_state.phy_addr, k_phy_spec_status, &status) < 0)
                return;
            bool link_up = (bmsr & k_bmsr_link) != 0;
            uint32 speed_code = (status & k_yt_speed_mask) >> k_yt_speed_shift;
            g_state.speed = speed_code == 2 ? 1000 : (speed_code == 1 ? 100 : 10);
            g_state.full_duplex = (status & k_yt_duplex) != 0;

            if (!g_state.link_reported || g_state.phy_link_up != link_up)
            {
                printf("[vf2_gmac] PHY link=%s bmsr=0x%x status=0x%x speed=%u duplex=%s\n",
                       link_up ? "up" : "down", bmsr, status, g_state.speed,
                       g_state.full_duplex ? "full" : "half");
                g_state.link_reported = true;
            }
            g_state.phy_link_up = link_up;
            uint32 config = mac_read(k_mac_configuration);
            config &= ~(k_mac_ps | k_mac_fes | k_mac_dm);
            if (g_state.speed < 1000)
                config |= k_mac_ps;
            if (g_state.speed == 100)
                config |= k_mac_fes;
            if (g_state.full_duplex)
                config |= k_mac_dm;
            mac_write(k_mac_configuration, config);
            vf2::platform_set_speed(g_state.speed);
            g_state.link_up = link_up;
        }

        bool init_rings()
        {
            memset(g_tx_ring, 0, sizeof(g_tx_ring));
            memset(g_rx_ring, 0, sizeof(g_rx_ring));
            memset(g_tx_buffers, 0, sizeof(g_tx_buffers));
            memset(g_rx_buffers, 0, sizeof(g_rx_buffers));
            for (uint32 i = 0; i < k_ring_count; ++i)
            {
                uint64 rx_pa = vf2::dma_address(g_rx_buffers[i]);
                g_rx_ring[i].des0 = static_cast<uint32>(rx_pa);
                g_rx_ring[i].des1 = static_cast<uint32>(rx_pa >> 32);
                g_rx_ring[i].des2 = 0;
                g_rx_ring[i].des3 = k_desc_own | k_desc_buf1_valid | k_desc_ioc;
            }
            return vf2::dma_sync_for_device(g_tx_ring, sizeof(g_tx_ring)) &&
                   vf2::dma_sync_for_device(g_rx_ring, sizeof(g_rx_ring)) &&
                   vf2::dma_sync_for_device(g_rx_buffers, sizeof(g_rx_buffers));
        }

        void reclaim_tx()
        {
            while (g_state.tx_head != g_state.tx_tail)
            {
                uint32 idx = g_state.tx_head % k_ring_count;
                if (!vf2::dma_sync_for_cpu(&g_tx_ring[idx], sizeof(g_tx_ring[idx])))
                {
                    ++g_state.tx_errors;
                    break;
                }
                if ((g_tx_ring[idx].des3 & k_desc_own) != 0)
                    break;
                if (g_state.tx_completed < 16)
                {
                    uint32 dma_status = mac_read(k_dma_ch0_status);
                    printf("[vf2_gmac] TX complete idx=%u des0=0x%x des1=0x%x des2=0x%x des3=0x%x desc_err=%u dma_status=0x%x\n",
                           idx, g_tx_ring[idx].des0, g_tx_ring[idx].des1,
                           g_tx_ring[idx].des2, g_tx_ring[idx].des3,
                           static_cast<uint32>((g_tx_ring[idx].des3 &
                                                k_tx_desc_error_summary) != 0),
                           dma_status);
                }
                memset(&g_tx_ring[idx], 0, sizeof(g_tx_ring[idx]));
                ++g_state.tx_head;
                ++g_state.tx_completed;
            }
        }
#endif
    }

    bool vf2_gmac_init()
    {
#ifndef VISIONFIVE2
        return false;
#else
        if (g_state.initialized)
            return true;
        g_state.lock.init("vf2_gmac");
        g_state.lock.acquire();
        if (!vf2::platform_init())
        {
            g_state.lock.release();
            return false;
        }
        // 先打开 GMAC 时钟再读取 U-Boot 写入的 MAC，避免设备仍处于
        // reset/clock-gated 状态时访问寄存器造成总线停顿。
        read_or_set_mac();

        mac_write(k_mac_configuration, 0);
        uint32 dma_mode_before = mac_read(k_dma_mode);
        mac_write(k_dma_mode, dma_mode_before | k_dma_swr);
        vf2::io_fence();
        printf("[vf2_gmac] DMA reset request mode_before=0x%x mode_after=0x%x\n",
               dma_mode_before, mac_read(k_dma_mode));
        if (!wait_dma_reset())
        {
            printf("[vf2_gmac] DMA reset timeout mode=0x%x status=0x%x sysbus=0x%x\n",
                   mac_read(k_dma_mode), mac_read(k_dma_ch0_status),
                   mac_read(k_dma_sysbus_mode));
            g_state.lock.release();
            return false;
        }

        mac_write(k_dma_sysbus_mode, k_dma_sysbus_fb | k_dma_sysbus_aal |
                                      (1u << 1) | (1u << 2) | (1u << 3));
        mac_write(k_mtl_txq0_operation_mode, (7u << 16) | (2u << 2) | (1u << 1));
        mac_write(k_mtl_rxq0_operation_mode, (7u << 20) | (1u << 5));
        mac_write(k_mac_rxq_ctrl0, 2u);
        mac_write(k_mac_packet_filter, 0);
        mac_write(k_dma_ch0_interrupt_enable, 0);

        mac_write(k_mac_address_low, static_cast<uint32>(g_state.mac[0]) |
                                    (static_cast<uint32>(g_state.mac[1]) << 8) |
                                    (static_cast<uint32>(g_state.mac[2]) << 16) |
                                    (static_cast<uint32>(g_state.mac[3]) << 24));
        mac_write(k_mac_address_high, static_cast<uint32>(g_state.mac[4]) |
                                     (static_cast<uint32>(g_state.mac[5]) << 8) |
                                     (1u << 31));

        if (!init_rings())
        {
            printf("[vf2_gmac] failed to synchronize DMA rings\n");
            g_state.lock.release();
            return false;
        }
        uint64 tx_pa = vf2::dma_address(g_tx_ring);
        uint64 rx_pa = vf2::dma_address(g_rx_ring);
        mac_write(k_dma_ch0_tx_desc_list_high, static_cast<uint32>(tx_pa >> 32));
        mac_write(k_dma_ch0_tx_desc_list, static_cast<uint32>(tx_pa));
        mac_write(k_dma_ch0_rx_desc_list_high, static_cast<uint32>(rx_pa >> 32));
        mac_write(k_dma_ch0_rx_desc_list, static_cast<uint32>(rx_pa));
        mac_write(k_dma_ch0_tx_ring_len, k_ring_count - 1);
        mac_write(k_dma_ch0_rx_ring_len, k_ring_count - 1);
        mac_write(k_dma_ch0_rx_tail, static_cast<uint32>(rx_pa + (k_ring_count - 1) * sizeof(DmaDescriptor)));
        mac_write(k_dma_ch0_rx_control, (k_rx_buffer_size << k_dma_rx_buf_size_shift) | (32u << 16));
        mac_write(k_dma_ch0_tx_control, (32u << 16) | k_dma_start);
        mac_write(k_dma_ch0_rx_control, (k_rx_buffer_size << k_dma_rx_buf_size_shift) | (32u << 16) | k_dma_start);

        bool phy_found = init_phy();
        update_link_state();
        uint32 config = mac_read(k_mac_configuration) | k_mac_te | k_mac_re;
        mac_write(k_mac_configuration, config);
        g_state.initialized = true;
        printf("[vf2_gmac] initialized mac=%02x:%02x:%02x:%02x:%02x:%02x phy=%s link=%s\n",
               g_state.mac[0], g_state.mac[1], g_state.mac[2], g_state.mac[3],
               g_state.mac[4], g_state.mac[5], phy_found ? "found" : "missing",
               g_state.link_up ? "up" : "down");
        g_state.lock.release();
        return true;
#endif
    }

    bool vf2_gmac_is_initialized() { return g_state.initialized; }

    int vf2_gmac_send(const void *data, uint32 len)
    {
#ifndef VISIONFIVE2
        (void)data; (void)len; return -1;
#else
        if (!g_state.initialized || data == nullptr || len == 0 || len > k_max_frame_size)
            return -1;
        g_state.lock.acquire();
        reclaim_tx();
        uint32 idx = g_state.tx_tail % k_ring_count;
        if ((g_tx_ring[idx].des3 & k_desc_own) != 0)
        {
            ++g_state.tx_errors;
            g_state.lock.release();
            return -11;
        }
        memcpy(g_tx_buffers[idx], data, len);
        if (!vf2::dma_sync_for_device(g_tx_buffers[idx], len))
        {
            ++g_state.tx_errors;
            g_state.lock.release();
            return -1;
        }
        uint64 pa = vf2::dma_address(g_tx_buffers[idx]);
        g_tx_ring[idx].des0 = static_cast<uint32>(pa);
        g_tx_ring[idx].des1 = static_cast<uint32>(pa >> 32);
        g_tx_ring[idx].des2 = len | k_tx_desc_ioc;
        vf2::io_fence();
        g_tx_ring[idx].des3 = k_desc_own | k_desc_fd | k_desc_ld | len;
        if (!vf2::dma_sync_for_device(&g_tx_ring[idx], sizeof(g_tx_ring[idx])))
        {
            ++g_state.tx_errors;
            g_state.lock.release();
            return -1;
        }
        ++g_state.tx_tail;
        mac_write(k_dma_ch0_tx_tail,
                  static_cast<uint32>(vf2::dma_address(&g_tx_ring[idx])));
        if (g_state.tx_packets < 16)
        {
            printf("[vf2_gmac] TX submit idx=%u len=%u ring_pa=0x%x buf_pa=0x%x des3=0x%x tail=0x%x\n",
                   idx, len, static_cast<uint32>(vf2::dma_address(&g_tx_ring[idx])),
                   static_cast<uint32>(pa), g_tx_ring[idx].des3,
                   mac_read(k_dma_ch0_tx_tail));
        }
        ++g_state.tx_packets;
        g_state.lock.release();
        return static_cast<int>(len);
#endif
    }

    int vf2_gmac_recv(void *data, uint32 *len)
    {
#ifndef VISIONFIVE2
        (void)data; (void)len; return -1;
#else
        if (!g_state.initialized || data == nullptr || len == nullptr || *len == 0)
            return -1;
        g_state.lock.acquire();
        uint32 idx = g_state.rx_head % k_ring_count;
        if (!vf2::dma_sync_for_cpu(&g_rx_ring[idx], sizeof(g_rx_ring[idx])))
        {
            ++g_state.rx_errors;
            g_state.lock.release();
            return -1;
        }
        if ((g_rx_ring[idx].des3 & k_desc_own) != 0)
        {
            g_state.lock.release();
            return -11;
        }
        uint32 packet_len = g_rx_ring[idx].des3 & k_desc_len_mask;
        if (packet_len == 0 || packet_len > k_rx_buffer_size ||
            packet_len > *len)
        {
            ++g_state.rx_errors;
            packet_len = 0;
        }
        if (packet_len != 0 &&
            !vf2::dma_sync_for_cpu(g_rx_buffers[idx], packet_len))
        {
            ++g_state.rx_errors;
            packet_len = 0;
        }
        if (packet_len != 0)
            memcpy(data, g_rx_buffers[idx], packet_len);
        *len = packet_len;
        uint64 pa = vf2::dma_address(g_rx_buffers[idx]);
        g_rx_ring[idx].des0 = static_cast<uint32>(pa);
        g_rx_ring[idx].des1 = static_cast<uint32>(pa >> 32);
        g_rx_ring[idx].des2 = 0;
        if (!vf2::dma_sync_for_device(g_rx_buffers[idx], k_rx_buffer_size))
        {
            ++g_state.rx_errors;
            g_state.lock.release();
            return -1;
        }
        g_rx_ring[idx].des3 = k_desc_own | k_desc_buf1_valid | k_desc_ioc;
        if (!vf2::dma_sync_for_device(&g_rx_ring[idx], sizeof(g_rx_ring[idx])))
        {
            ++g_state.rx_errors;
            g_state.lock.release();
            return -1;
        }
        mac_write(k_dma_ch0_rx_tail, static_cast<uint32>(vf2::dma_address(&g_rx_ring[idx])));
        ++g_state.rx_head;
        if (packet_len != 0)
            ++g_state.rx_packets;
        g_state.lock.release();
        return packet_len == 0 ? -1 : static_cast<int>(packet_len);
#endif
    }

    void vf2_gmac_poll()
    {
#ifdef VISIONFIVE2
        if (!g_state.initialized)
            return;
        g_state.lock.acquire();
        reclaim_tx();
        update_link_state();
        (void)mac_read(k_dma_ch0_status);
        g_state.lock.release();
#endif
    }

    void vf2_gmac_intr() { vf2_gmac_poll(); }
    bool vf2_gmac_link_up() { return g_state.initialized && g_state.link_up; }

    void vf2_gmac_get_mac(uint8 mac[6])
    {
        if (mac != nullptr)
            memcpy(mac, g_state.mac, 6);
    }

    void vf2_gmac_debug_status()
    {
#ifdef VISIONFIVE2
        printf("[vf2_gmac] init=%d link=%d speed=%u tx=%u/%u rx=%u status=0x%x txp=%llu txc=%llu rxp=%llu txe=%llu rxe=%llu\n",
               g_state.initialized, g_state.link_up, g_state.speed, g_state.tx_head,
               g_state.tx_tail, g_state.rx_head, vf2::read_reg(VF2_GMAC1_BASE_V, k_dma_ch0_status),
               g_state.tx_packets, g_state.tx_completed, g_state.rx_packets,
               g_state.tx_errors, g_state.rx_errors);
#endif
    }
}

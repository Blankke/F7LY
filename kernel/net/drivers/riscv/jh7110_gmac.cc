/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * StarFive JH7110 GMAC1 polling driver for VisionFive 2.
 *
 * Based on the RocketOS VisionFive 2 driver (revision
 * 1e1fc238f6ffa17b262163f029d48f660943cd96) and the former F7LY
 * visionfive2 branch. Descriptor, MDIO and PHY details were revalidated
 * against Linux stmmac/dwmac4 and motorcomm PHY drivers. This version is
 * intentionally polling-only; IRQ78 is not registered in the first stage.
 */
#include "jh7110_gmac.hh"

#include "devs/dtb.hh"
#include "devs/spinlock.hh"
#include "gmac_control.hh"
#include "jh7110_gmac_regs.hh"
#include "libs/klib.hh"
#include "net/drivers/platform_net_device.hh"
#include "printer.hh"
#include "tm/time.hh"

namespace riscv::jh7110::gmac
{
namespace
{
using namespace regs;
namespace control = platform_control;

constexpr uint32 k_dma_buffer_size = 2048;
constexpr uint32 k_ethernet_fcs_size = 4;
constexpr uint64 k_dma_reset_timeout_us = 1'000'000;
constexpr uint64 k_mdio_timeout_us = 100'000;
constexpr uint64 k_phy_reset_timeout_us = 500'000;
constexpr uint64 k_phy_poll_interval_us = 250'000;
constexpr uint16 k_invalid_phy_address = 0xffff;

// JH7110 的 GMAC1 不是 dma-coherent。每个 descriptor 独占一个 cache line，
// 避免 CPU flush 本描述符时把同一行内 DMA 刚写回的另一个描述符覆盖掉。
// DWMAC4 固定读取前 16 bytes；其余 48 bytes 只负责隔离 cache line。
struct alignas(64) DescriptorSlot
{
    DmaDescriptor descriptor;
    uint8 cache_line_padding[64 - sizeof(DmaDescriptor)];
};

static_assert(sizeof(DescriptorSlot) == 64,
              "one descriptor must own exactly one cache line");
static_assert(alignof(DescriptorSlot) == 64,
              "descriptor slot must be cache-line aligned");

alignas(64) DescriptorSlot g_tx_slot{};
alignas(64) DescriptorSlot g_rx_slot{};
alignas(64) uint8 g_tx_buffer[k_dma_buffer_size]{};
alignas(64) uint8 g_rx_buffer[k_dma_buffer_size]{};
static_assert(sizeof(g_tx_buffer) % 64 == 0 &&
                  sizeof(g_rx_buffer) % 64 == 0,
              "DMA buffers must cover complete cache lines");

struct DriverState
{
    bool initialized = false;
    bool link_known = false;
    bool negotiated_link_up = false;
    bool link_up = false;
    bool full_duplex = false;
    bool phy_io_error_logged = false;
    bool tx_in_flight = false;
    uint16 phy_address = k_invalid_phy_address;
    uint32 speed_mbps = 0;
    uint64 last_phy_poll_ticks = 0;
    uint64 tx_descriptor_physical = 0;
    uint64 rx_descriptor_physical = 0;
    uint64 tx_buffer_physical = 0;
    uint64 rx_buffer_physical = 0;
    uint64 tx_packets = 0;
    uint64 rx_packets = 0;
    uint64 tx_errors = 0;
    uint64 rx_errors = 0;
    uint64 tx_busy = 0;
    uint32 last_dma_status = 0;
    uint32 last_reported_dma_error = 0;
    uint8 mac[6]{};
};

DriverState g_state{};
SpinLock g_lock;
bool g_lock_ready = false;

bool valid_mac(const uint8 mac[6])
{
    if (mac == nullptr || (mac[0] & 1U) != 0)
    {
        return false;
    }
    bool any_nonzero = false;
    bool any_not_ff = false;
    for (uint32 index = 0; index < 6; ++index)
    {
        any_nonzero = any_nonzero || mac[index] != 0;
        any_not_ff = any_not_ff || mac[index] != 0xff;
    }
    return any_nonzero && any_not_ff;
}

bool select_mac_address()
{
    uint8 candidate[6]{};
    if (DtbManager::get_mac_address(control::device_physical_address(),
                                    candidate) &&
        valid_mac(candidate))
    {
        memcpy(g_state.mac, candidate, sizeof(g_state.mac));
        platformDiagnosticInfo(
            "[gmac1] MAC from DTB %02x:%02x:%02x:%02x:%02x:%02x\n",
            candidate[0], candidate[1], candidate[2], candidate[3],
            candidate[4], candidate[5]);
        return true;
    }

    // DTB 缺少地址时才读取 U-Boot 已写入 MAC address0 寄存器。
    const uint32 low = control::read(k_mac_address0_low);
    const uint32 high = control::read(k_mac_address0_high);
    candidate[0] = static_cast<uint8>(low);
    candidate[1] = static_cast<uint8>(low >> 8);
    candidate[2] = static_cast<uint8>(low >> 16);
    candidate[3] = static_cast<uint8>(low >> 24);
    candidate[4] = static_cast<uint8>(high);
    candidate[5] = static_cast<uint8>(high >> 8);
    if (!valid_mac(candidate))
    {
        // 固定 fallback 会让多块板得到同一个 MAC，引发二层地址冲突；
        // 因此两个板级来源都无效时必须明确失败。
        platformDiagnosticError(
            "[gmac1] no valid MAC in DTB or U-Boot registers low=0x%x high=0x%x\n",
            low, high);
        return false;
    }

    memcpy(g_state.mac, candidate, sizeof(g_state.mac));
    platformDiagnosticWarn(
        "[gmac1] DTB has no MAC, using U-Boot register %02x:%02x:%02x:%02x:%02x:%02x\n",
        candidate[0], candidate[1], candidate[2], candidate[3], candidate[4],
        candidate[5]);
    return true;
}

bool wait_until_clear(uint32 offset, uint32 mask, uint64 timeout_us)
{
    const uint64 start = tmm::get_hw_time_stamp();
    const uint64 timeout = tmm::microseconds_to_cycles(timeout_us);
    while ((control::read(offset) & mask) != 0)
    {
        if (tmm::get_hw_time_stamp() - start >= timeout)
        {
            return false;
        }
        asm volatile("nop");
    }
    return true;
}

bool reset_dma()
{
    const uint32 before = control::read(k_dma_mode);
    control::write(k_dma_mode, before | k_dma_software_reset);
    control::io_barrier();
    if (wait_until_clear(k_dma_mode, k_dma_software_reset,
                         k_dma_reset_timeout_us))
    {
        return true;
    }
    platformDiagnosticError(
        "[gmac1] DMA reset timeout mode=0x%x channel-status=0x%x sysbus=0x%x\n",
        control::read(k_dma_mode), control::read(k_dma_channel0_status),
        control::read(k_dma_sysbus_mode));
    return false;
}

bool mdio_wait_idle()
{
    return wait_until_clear(k_mac_mdio_address, k_mdio_busy,
                            k_mdio_timeout_us);
}

bool mdio_read(uint16 phy, uint16 reg, uint16 &value)
{
    if (phy > 31 || reg > 31 || !mdio_wait_idle())
    {
        return false;
    }
    const uint32 command =
        (static_cast<uint32>(phy) << k_mdio_phy_shift) |
        (static_cast<uint32>(reg) << k_mdio_register_shift) |
        (k_mdio_clock_range_250_300 << k_mdio_clock_range_shift) |
        (k_mdio_goc_read << k_mdio_goc_shift) | k_mdio_busy;
    control::write(k_mac_mdio_address, command);
    control::io_barrier();
    if (!mdio_wait_idle())
    {
        return false;
    }
    value = static_cast<uint16>(control::read(k_mac_mdio_data));
    return true;
}

bool mdio_write(uint16 phy, uint16 reg, uint16 value)
{
    if (phy > 31 || reg > 31 || !mdio_wait_idle())
    {
        return false;
    }
    control::write(k_mac_mdio_data, value);
    const uint32 command =
        (static_cast<uint32>(phy) << k_mdio_phy_shift) |
        (static_cast<uint32>(reg) << k_mdio_register_shift) |
        (k_mdio_clock_range_250_300 << k_mdio_clock_range_shift) |
        (k_mdio_goc_write << k_mdio_goc_shift) | k_mdio_busy;
    control::write(k_mac_mdio_address, command);
    control::io_barrier();
    return mdio_wait_idle();
}

bool phy_extended_read(uint16 reg, uint16 &value)
{
    return mdio_write(g_state.phy_address, k_phy_extended_address, reg) &&
           mdio_read(g_state.phy_address, k_phy_extended_data, value);
}

bool phy_extended_write(uint16 reg, uint16 value)
{
    return mdio_write(g_state.phy_address, k_phy_extended_address, reg) &&
           mdio_write(g_state.phy_address, k_phy_extended_data, value);
}

bool initialize_phy()
{
    g_state.phy_address = k_invalid_phy_address;
    uint32 first_other_id = 0;
    uint16 first_other_address = k_invalid_phy_address;
    for (uint16 phy = 0; phy < 32; ++phy)
    {
        uint16 id1 = 0;
        uint16 id2 = 0;
        if (!mdio_read(phy, k_phy_id1, id1) ||
            !mdio_read(phy, k_phy_id2, id2))
        {
            platformDiagnosticError(
                "[gmac1] MDIO transaction failed while scanning PHY address=%u\n",
                phy);
            return false;
        }
        if (id1 == 0 || id1 == 0xffff || id2 == 0 || id2 == 0xffff)
        {
            continue;
        }
        const uint32 id = (static_cast<uint32>(id1) << 16) | id2;
        if (id == k_yt8531_id)
        {
            g_state.phy_address = phy;
            break;
        }
        if (first_other_address == k_invalid_phy_address)
        {
            first_other_address = phy;
            first_other_id = id;
        }
    }
    if (g_state.phy_address == k_invalid_phy_address)
    {
        if (first_other_address != k_invalid_phy_address)
        {
            platformDiagnosticError(
                "[gmac1] unsupported PHY address=%u id=0x%x; expected YT8531 id=0x%x\n",
                first_other_address, first_other_id, k_yt8531_id);
        }
        else
        {
            platformDiagnosticError("[gmac1] no PHY responded on MDIO bus\n");
        }
        return false;
    }

    platformDiagnosticInfo("[gmac1] YT8531 PHY address=%u id=0x%x\n",
                           g_state.phy_address, k_yt8531_id);

    uint16 bmcr = 0;
    if (!mdio_read(g_state.phy_address, k_phy_bmcr, bmcr) ||
        !mdio_write(g_state.phy_address, k_phy_bmcr,
                    static_cast<uint16>(bmcr | k_bmcr_reset)))
    {
        platformDiagnosticError("[gmac1] failed to request PHY reset\n");
        return false;
    }

    const uint64 reset_start = tmm::get_hw_time_stamp();
    const uint64 reset_timeout =
        tmm::microseconds_to_cycles(k_phy_reset_timeout_us);
    bool reset_complete = false;
    while (tmm::get_hw_time_stamp() - reset_start < reset_timeout)
    {
        if (!mdio_read(g_state.phy_address, k_phy_bmcr, bmcr))
        {
            platformDiagnosticError("[gmac1] MDIO failed during PHY reset\n");
            return false;
        }
        if ((bmcr & k_bmcr_reset) == 0)
        {
            reset_complete = true;
            break;
        }
        control::delay_us(1000);
    }
    if (!reset_complete)
    {
        platformDiagnosticError("[gmac1] PHY reset bit did not clear\n");
        return false;
    }

    // 官方板级 DTS 使用 rgmii-id。Linux motorcomm 默认在 RX/TX 两侧各
    // 配置 1.95 ns，并清除额外的 1.9 ns RXC 开关，避免叠加成双重延时。
    uint16 extended = 0;
    if (!phy_extended_read(k_yt_chip_config, extended) ||
        !phy_extended_write(
            k_yt_chip_config,
            static_cast<uint16>(extended & ~k_yt_rxc_delay_enable)))
    {
        platformDiagnosticError("[gmac1] failed to configure PHY RX delay\n");
        return false;
    }
    if (!phy_extended_read(k_yt_rgmii_config, extended))
    {
        platformDiagnosticError("[gmac1] failed to read PHY RGMII delay\n");
        return false;
    }
    extended = static_cast<uint16>(
        (extended & ~(k_yt_rx_delay_mask | k_yt_ge_tx_delay_mask)) |
        (k_yt_delay_1_95_ns << 10) | k_yt_delay_1_95_ns);
    if (!phy_extended_write(k_yt_rgmii_config, extended))
    {
        platformDiagnosticError("[gmac1] failed to write PHY RGMII delay\n");
        return false;
    }

    if (!mdio_write(g_state.phy_address, k_phy_advertise,
                    k_phy_advertise_default) ||
        !mdio_write(g_state.phy_address, k_phy_ctrl1000,
                    k_phy_advertise_1000) ||
        !mdio_write(g_state.phy_address, k_phy_bmcr,
                    k_bmcr_autoneg_enable | k_bmcr_autoneg_restart))
    {
        platformDiagnosticError(
            "[gmac1] failed to configure/restart PHY autonegotiation\n");
        return false;
    }
    return true;
}

void configure_mac_link(uint32 speed_mbps, bool full_duplex)
{
    uint32 configuration = control::read(k_mac_configuration);
    configuration &=
        ~(k_mac_port_select | k_mac_fast_ethernet_speed | k_mac_duplex);
    if (speed_mbps == 100)
    {
        configuration |= k_mac_port_select | k_mac_fast_ethernet_speed;
    }
    else if (speed_mbps == 10)
    {
        configuration |= k_mac_port_select;
    }
    if (full_duplex)
    {
        configuration |= k_mac_duplex;
    }
    control::write(k_mac_configuration, configuration);
}

uint32 decode_phy_speed(uint16 status)
{
    switch (status & k_yt_status_speed_mask)
    {
    case k_yt_status_speed_1000:
        return 1000;
    case k_yt_status_speed_100:
        return 100;
    case 0:
        return 10;
    default:
        return 0;
    }
}

bool update_phy_state_locked(bool force)
{
    const uint64 now = tmm::get_hw_time_stamp();
    const uint64 interval =
        tmm::microseconds_to_cycles(k_phy_poll_interval_us);
    if (!force && now - g_state.last_phy_poll_ticks < interval)
    {
        return true;
    }
    g_state.last_phy_poll_ticks = now;

    uint16 status = 0;
    if (!mdio_read(g_state.phy_address, k_phy_specific_status, status))
    {
        if (!g_state.phy_io_error_logged)
        {
            platformDiagnosticError(
                "[gmac1] MDIO failed while polling PHY status\n");
        }
        g_state.phy_io_error_logged = true;
        g_state.link_up = false;
        return !force;
    }

    const bool recovered_from_error = g_state.phy_io_error_logged;
    g_state.phy_io_error_logged = false;
    const bool physical_link =
        (status & (k_yt_status_link | k_yt_status_resolved)) ==
        (k_yt_status_link | k_yt_status_resolved);
    const uint32 speed = physical_link ? decode_phy_speed(status) : 0;
    const bool full_duplex = physical_link &&
                             (status & k_yt_status_duplex) != 0;
    const bool changed = !g_state.link_known || recovered_from_error ||
                         physical_link != g_state.negotiated_link_up ||
                         speed != g_state.speed_mbps ||
                         full_duplex != g_state.full_duplex;
    if (!changed)
    {
        return true;
    }

    bool usable_link = physical_link;
    if (physical_link)
    {
        if (speed == 0 || !control::set_speed(speed))
        {
            usable_link = false;
        }
        else
        {
            configure_mac_link(speed, full_duplex);
        }
    }

    g_state.link_known = true;
    g_state.negotiated_link_up = physical_link;
    g_state.link_up = usable_link;
    g_state.speed_mbps = speed;
    g_state.full_duplex = full_duplex;
    if (!physical_link)
    {
        platformDiagnosticInfo("[gmac1] link down status=0x%x\n", status);
    }
    else if (!usable_link)
    {
        platformDiagnosticWarn(
            "[gmac1] PHY negotiated %u Mbps but platform clock is unsupported; traffic disabled\n",
            speed);
    }
    else
    {
        platformDiagnosticInfo("[gmac1] link up %u Mbps %s-duplex\n", speed,
                               full_duplex ? "full" : "half");
    }
    return true;
}

bool map_32bit_dma(const void *pointer, uint64 size, uint64 &physical,
                   const char *name)
{
    if (!control::dma_address(pointer, size, physical) ||
        physical > 0xffffffffULL ||
        size - 1 > 0xffffffffULL - physical)
    {
        platformDiagnosticError(
            "[gmac1] %s is not a contiguous 32-bit DMA range va=%p size=%lu pa=0x%lx\n",
            name, pointer, size, physical);
        return false;
    }
    if ((physical & 63U) != 0)
    {
        platformDiagnosticError(
            "[gmac1] %s physical address is not cache-line aligned pa=0x%lx\n",
            name, physical);
        return false;
    }
    return true;
}

bool prepare_dma_storage()
{
    if (!map_32bit_dma(&g_tx_slot, sizeof(g_tx_slot),
                       g_state.tx_descriptor_physical, "TX descriptor") ||
        !map_32bit_dma(&g_rx_slot, sizeof(g_rx_slot),
                       g_state.rx_descriptor_physical, "RX descriptor") ||
        !map_32bit_dma(g_tx_buffer, sizeof(g_tx_buffer),
                       g_state.tx_buffer_physical, "TX buffer") ||
        !map_32bit_dma(g_rx_buffer, sizeof(g_rx_buffer),
                       g_state.rx_buffer_physical, "RX buffer"))
    {
        return false;
    }

    memset(&g_tx_slot, 0, sizeof(g_tx_slot));
    memset(&g_rx_slot, 0, sizeof(g_rx_slot));
    memset(g_tx_buffer, 0, sizeof(g_tx_buffer));
    memset(g_rx_buffer, 0, sizeof(g_rx_buffer));

    DmaDescriptor &rx = g_rx_slot.descriptor;
    rx.des0 = static_cast<uint32>(g_state.rx_buffer_physical);
    rx.des1 = 0;
    rx.des2 = 0;
    rx.des3 = k_rx_owned_by_dma | k_rx_buffer1_valid |
              k_rx_interrupt_on_completion;

    // CPU -> DMA 所有权顺序：buffer 先可见，descriptor(含 OWN) 后可见。
    return control::dma_sync_for_device(g_tx_buffer, sizeof(g_tx_buffer)) &&
           control::dma_sync_for_device(g_rx_buffer, sizeof(g_rx_buffer)) &&
           control::dma_sync_for_device(&g_tx_slot, sizeof(g_tx_slot)) &&
           control::dma_sync_for_device(&g_rx_slot, sizeof(g_rx_slot));
}

void program_mac_address()
{
    const uint32 low = static_cast<uint32>(g_state.mac[0]) |
                       (static_cast<uint32>(g_state.mac[1]) << 8) |
                       (static_cast<uint32>(g_state.mac[2]) << 16) |
                       (static_cast<uint32>(g_state.mac[3]) << 24);
    const uint32 high = static_cast<uint32>(g_state.mac[4]) |
                        (static_cast<uint32>(g_state.mac[5]) << 8) |
                        k_mac_address_enable;
    control::write(k_mac_address0_low, low);
    control::write(k_mac_address0_high, high);
}

void program_dma_and_queues()
{
    control::write(k_dma_mode,
                   control::read(k_dma_mode) |
                       k_dma_descriptor_cache_enable);
    control::write(
        k_dma_sysbus_mode,
        k_dma_sysbus_fixed_burst | k_dma_sysbus_blen32 |
            k_dma_sysbus_blen64 | k_dma_sysbus_blen128 |
            k_dma_sysbus_blen256 | k_dma_sysbus_enhanced_addressing |
            k_dma_sysbus_address_aligned_beats | k_dma_sysbus_read_osr_15 |
            k_dma_sysbus_write_osr_15);
    control::write(k_mtl_txq0_operation_mode,
                   k_mtl_tx_queue_size_2048 | k_mtl_tx_queue_enable |
                       k_mtl_tx_store_and_forward);
    control::write(k_mtl_rxq0_operation_mode,
                   k_mtl_rx_queue_size_2048 |
                       k_mtl_rx_store_and_forward);
    control::write(k_mac_rxq_ctrl0, k_mac_rxq0_enable_dcb);
    control::write(k_mac_packet_filter, 0);
    control::write(k_dma_channel0_control, 0);
    control::write(k_dma_channel0_interrupt_enable, 0);

    control::write(k_dma_channel0_tx_desc_list_high, 0);
    control::write(k_dma_channel0_tx_desc_list,
                   static_cast<uint32>(g_state.tx_descriptor_physical));
    control::write(k_dma_channel0_rx_desc_list_high, 0);
    control::write(k_dma_channel0_rx_desc_list,
                   static_cast<uint32>(g_state.rx_descriptor_physical));
    control::write(k_dma_channel0_tx_ring_length, 0);
    control::write(k_dma_channel0_rx_ring_length, 0);

    // Tail 是右开边界/doorbell，不是“当前 descriptor”。Linux RX 初始化写
    // base + count*16，U-Boot EQOS 发送也写下一个 descriptor。单描述符
    // one-shot 因此以 base 表示空 TX，以 base+16 发布唯一 TX/RX descriptor。
    // DMA 写回 OWN=0 后回绕到 base 并暂停；CPU 归还后重写 base+16 唤醒。
    control::write(k_dma_channel0_tx_tail,
                   static_cast<uint32>(g_state.tx_descriptor_physical));
    control::write(
        k_dma_channel0_rx_tail,
        static_cast<uint32>(g_state.rx_descriptor_physical +
                            sizeof(DmaDescriptor)));

    control::write(k_dma_channel0_tx_control,
                   k_dma_tx_pbl_16 | k_dma_tx_operate_on_second_packet);
    control::write(k_dma_channel0_rx_control,
                   (k_dma_buffer_size << k_dma_rx_buffer_size_shift) |
                       k_dma_rx_pbl_16);

    const uint32 pending = control::read(k_dma_channel0_status);
    if (pending != 0)
    {
        control::write(k_dma_channel0_status, pending);
    }
    control::io_barrier();
}

void stop_hardware()
{
    control::write(k_dma_channel0_tx_control,
                   control::read(k_dma_channel0_tx_control) &
                       ~k_dma_channel_start);
    control::write(k_dma_channel0_rx_control,
                   control::read(k_dma_channel0_rx_control) &
                       ~k_dma_channel_start);
    control::write(k_mac_configuration,
                   control::read(k_mac_configuration) &
                       ~(k_mac_transmit_enable | k_mac_receive_enable));
    control::io_barrier();
}

bool reclaim_tx_locked()
{
    if (!g_state.tx_in_flight)
    {
        return true;
    }
    // DMA -> CPU 前必须 invalidate descriptor；仅 fence 会一直读到旧 OWN。
    if (!control::dma_sync_for_cpu(&g_tx_slot, sizeof(g_tx_slot)))
    {
        ++g_state.tx_errors;
        return false;
    }
    const uint32 status = g_tx_slot.descriptor.des3;
    if ((status & k_tx_owned_by_dma) != 0)
    {
        return true;
    }

    g_state.tx_in_flight = false;
    if ((status & k_tx_error_summary) != 0)
    {
        ++g_state.tx_errors;
        if (g_state.tx_errors <= 4 ||
            (g_state.tx_errors & (g_state.tx_errors - 1)) == 0)
        {
            platformDiagnosticWarn(
                "[gmac1] TX descriptor completed with error status=0x%x count=%lu\n",
                status, g_state.tx_errors);
        }
    }
    else
    {
        ++g_state.tx_packets;
    }
    return true;
}

bool rearm_rx_locked()
{
    // CPU 不修改 RX payload，但仍需丢弃 CPU 可能保留的 clean cache line，
    // 再把 buffer 交还给非一致性 DMA。
    if (!control::dma_sync_for_device(g_rx_buffer, sizeof(g_rx_buffer)))
    {
        return false;
    }
    DmaDescriptor &descriptor = g_rx_slot.descriptor;
    descriptor.des0 = static_cast<uint32>(g_state.rx_buffer_physical);
    descriptor.des1 = 0;
    descriptor.des2 = 0;
    descriptor.des3 = k_rx_owned_by_dma | k_rx_buffer1_valid |
                      k_rx_interrupt_on_completion;
    if (!control::dma_sync_for_device(&g_rx_slot, sizeof(g_rx_slot)))
    {
        return false;
    }
    control::write(
        k_dma_channel0_rx_tail,
        static_cast<uint32>(g_state.rx_descriptor_physical +
                            sizeof(DmaDescriptor)));
    control::io_barrier();
    return true;
}

void inspect_dma_status_locked()
{
    const uint32 status = control::read(k_dma_channel0_status);
    g_state.last_dma_status = status;
    const uint32 serious =
        status & (k_dma_status_context_descriptor_error |
                  k_dma_status_fatal_bus_error);
    if (serious != 0 && serious != g_state.last_reported_dma_error)
    {
        platformDiagnosticError(
            "[gmac1] DMA channel error status=0x%x\n", status);
    }
    if (serious != 0)
    {
        g_state.last_reported_dma_error = serious;
    }
    if (status != 0)
    {
        // 轮询模式不消费 IRQ；W1C 掉状态，RBU/TBU 在 one-shot 暂停时正常。
        control::write(k_dma_channel0_status, status);
    }
}

void reset_software_state()
{
    g_state.initialized = false;
    g_state.link_known = false;
    g_state.negotiated_link_up = false;
    g_state.link_up = false;
    g_state.full_duplex = false;
    g_state.phy_io_error_logged = false;
    g_state.tx_in_flight = false;
    g_state.phy_address = k_invalid_phy_address;
    g_state.speed_mbps = 0;
    g_state.last_phy_poll_ticks = 0;
    g_state.tx_descriptor_physical = 0;
    g_state.rx_descriptor_physical = 0;
    g_state.tx_buffer_physical = 0;
    g_state.rx_buffer_physical = 0;
    g_state.tx_packets = 0;
    g_state.rx_packets = 0;
    g_state.tx_errors = 0;
    g_state.rx_errors = 0;
    g_state.tx_busy = 0;
    g_state.last_dma_status = 0;
    g_state.last_reported_dma_error = 0;
    memset(g_state.mac, 0, sizeof(g_state.mac));
}
} // namespace

bool initialize()
{
    if (!g_lock_ready)
    {
        g_lock.init("jh7110_gmac1");
        g_lock_ready = true;
    }
    g_lock.acquire();
    if (g_state.initialized)
    {
        g_lock.release();
        return true;
    }
    reset_software_state();

    if (!control::initialize() || !select_mac_address())
    {
        g_lock.release();
        return false;
    }

    control::write(k_mac_configuration, 0);
    control::write(k_dma_channel0_tx_control, 0);
    control::write(k_dma_channel0_rx_control, 0);
    control::io_barrier();
    if (!reset_dma() || !prepare_dma_storage())
    {
        stop_hardware();
        g_lock.release();
        return false;
    }

    program_dma_and_queues();
    program_mac_address();
    configure_mac_link(1000, true);
    if (!initialize_phy() || !update_phy_state_locked(true))
    {
        stop_hardware();
        g_lock.release();
        return false;
    }

    control::write(k_dma_channel0_tx_control,
                   control::read(k_dma_channel0_tx_control) |
                       k_dma_channel_start);
    control::write(k_dma_channel0_rx_control,
                   control::read(k_dma_channel0_rx_control) |
                       k_dma_channel_start);
    control::write(k_mac_configuration,
                   control::read(k_mac_configuration) |
                       k_mac_transmit_enable | k_mac_receive_enable);
    control::io_barrier();
    g_state.initialized = true;

    platformDiagnosticInfo(
        "[gmac1] initialized polling-only tx-desc=0x%lx rx-desc=0x%lx tx-buf=0x%lx rx-buf=0x%lx\n",
        g_state.tx_descriptor_physical, g_state.rx_descriptor_physical,
        g_state.tx_buffer_physical, g_state.rx_buffer_physical);
    g_lock.release();
    return true;
}

int send(const void *data, uint32 length)
{
    if (data == nullptr || length == 0 ||
        length > net::platform_device::k_max_ethernet_frame || !g_lock_ready)
    {
        return -1;
    }

    g_lock.acquire();
    if (!g_state.initialized || !g_state.link_up || !reclaim_tx_locked())
    {
        g_lock.release();
        return -1;
    }
    if (g_state.tx_in_flight)
    {
        ++g_state.tx_busy;
        g_lock.release();
        return -1;
    }

    memcpy(g_tx_buffer, data, length);
    if (!control::dma_sync_for_device(g_tx_buffer, sizeof(g_tx_buffer)))
    {
        ++g_state.tx_errors;
        g_lock.release();
        return -1;
    }

    DmaDescriptor &descriptor = g_tx_slot.descriptor;
    descriptor.des0 = static_cast<uint32>(g_state.tx_buffer_physical);
    descriptor.des1 = 0;
    descriptor.des2 = (length & k_tx_buffer1_size_mask) |
                      k_tx_interrupt_on_completion;
    descriptor.des3 = (length & k_tx_packet_size_mask) |
                      k_tx_first_descriptor | k_tx_last_descriptor |
                      k_tx_owned_by_dma;
    // OWN 随整个独占 slot 最后一次 writeback 给 DMA；随后 tail 才发布任务。
    if (!control::dma_sync_for_device(&g_tx_slot, sizeof(g_tx_slot)))
    {
        ++g_state.tx_errors;
        g_lock.release();
        return -1;
    }
    g_state.tx_in_flight = true;
    control::write(
        k_dma_channel0_tx_tail,
        static_cast<uint32>(g_state.tx_descriptor_physical +
                            sizeof(DmaDescriptor)));
    control::io_barrier();
    g_lock.release();
    // net::backend 契约以 0 表示成功，不能返回发送字节数。
    return 0;
}

int receive(void *data, uint32 *length)
{
    if (length == nullptr)
    {
        return -1;
    }
    const uint32 capacity = *length;
    *length = 0;
    if (data == nullptr || capacity == 0 || !g_lock_ready)
    {
        return -1;
    }

    g_lock.acquire();
    if (!g_state.initialized)
    {
        g_lock.release();
        return -1;
    }
    if (!g_state.link_up)
    {
        // 100/10 Mbit 的板级时钟尚未验证时，收发两侧都保持关闭；不能只
        // 阻止发送，却把使用错误 RX 时钟得到的数据继续交给协议栈。
        g_lock.release();
        return 0;
    }
    // 每一次读 OWN 前都 invalidate 整个独占 descriptor cache line。
    if (!control::dma_sync_for_cpu(&g_rx_slot, sizeof(g_rx_slot)))
    {
        ++g_state.rx_errors;
        g_lock.release();
        return -1;
    }
    const uint32 status = g_rx_slot.descriptor.des3;
    if ((status & k_rx_owned_by_dma) != 0)
    {
        g_lock.release();
        // 无包也是一次成功查询，通过 length=0 表达。
        return 0;
    }

    const uint32 raw_length = status & k_rx_packet_size_mask;
    bool valid = (status & k_rx_first_descriptor) != 0 &&
                 (status & k_rx_last_descriptor) != 0 &&
                 (status & k_rx_context_descriptor) == 0 &&
                 (status & k_rx_hardware_error_mask) == 0 &&
                 raw_length >= k_ethernet_fcs_size &&
                 raw_length <= sizeof(g_rx_buffer);
    const uint32 packet_length =
        valid ? raw_length - k_ethernet_fcs_size : 0;
    valid = valid && packet_length != 0 && packet_length <= capacity;

    if (valid)
    {
        // descriptor 已证明 DMA 放弃 OWN 后，才允许 invalidate/read payload。
        valid = control::dma_sync_for_cpu(g_rx_buffer, sizeof(g_rx_buffer));
    }
    if (valid)
    {
        memcpy(data, g_rx_buffer, packet_length);
        *length = packet_length;
        ++g_state.rx_packets;
    }
    else
    {
        ++g_state.rx_errors;
        // 坏包可能连续到达；保留前四次和 2^n 次样本，避免串口刷屏。
        if (g_state.rx_errors <= 4 ||
            (g_state.rx_errors & (g_state.rx_errors - 1)) == 0)
        {
            platformDiagnosticWarn(
                "[gmac1] dropped RX descriptor status=0x%x raw-len=%u capacity=%u count=%lu\n",
                status, raw_length, capacity, g_state.rx_errors);
        }
    }

    if (!rearm_rx_locked())
    {
        ++g_state.rx_errors;
        *length = 0;
        g_lock.release();
        return -1;
    }
    g_lock.release();
    return valid ? 0 : -1;
}

void poll()
{
    if (!g_lock_ready)
    {
        return;
    }
    g_lock.acquire();
    if (g_state.initialized)
    {
        (void)reclaim_tx_locked();
        (void)update_phy_state_locked(false);
        inspect_dma_status_locked();
    }
    g_lock.release();
}

void get_mac(uint8 mac[6])
{
    if (mac == nullptr)
    {
        return;
    }
    if (!g_lock_ready)
    {
        memset(mac, 0, 6);
        return;
    }
    g_lock.acquire();
    memcpy(mac, g_state.mac, 6);
    g_lock.release();
}

void debug_status()
{
    if (!g_lock_ready)
    {
        platformDiagnosticInfo("[gmac1] driver not initialized\n");
        return;
    }
    g_lock.acquire();
    platformDiagnosticInfo(
        "[gmac1] init=%u link=%u negotiated=%u speed=%u duplex=%u phy=%u tx-flight=%u tx=%lu rx=%lu txerr=%lu rxerr=%lu busy=%lu dma=0x%x\n",
        g_state.initialized, g_state.link_up, g_state.negotiated_link_up,
        g_state.speed_mbps, g_state.full_duplex, g_state.phy_address,
        g_state.tx_in_flight, g_state.tx_packets, g_state.rx_packets,
        g_state.tx_errors, g_state.rx_errors, g_state.tx_busy,
        g_state.last_dma_status);
    g_lock.release();
}
} // namespace riscv::jh7110::gmac

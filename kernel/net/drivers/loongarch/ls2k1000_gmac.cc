#include "ls2k1000_gmac.hh"

#ifdef BOARD_LS2K1000

#include "devs/dtb.hh"
#include "hal/loongarch/platform_board.hh"
#include "libs/klib.hh"
#include "platform.hh"
#include "printer.hh"
#include "spinlock.hh"
#include "tm/time.hh"

namespace loongarch::ls2k1000::gmac
{
namespace
{
constexpr uint32 k_ring_size = 128;
constexpr uint32 k_buffer_size = 2048;
constexpr uint32 k_ethernet_fcs_size = 4;
constexpr uint64 k_dma_reset_timeout_us = 1'000'000;

constexpr uint32 k_mac_config = 0x0000;
constexpr uint32 k_mac_frame_filter = 0x0004;
constexpr uint32 k_mac_flow_control = 0x0018;
constexpr uint32 k_mac_version = 0x0020;
constexpr uint32 k_mac_interrupt_status = 0x0038;
constexpr uint32 k_mac_interrupt_mask = 0x003c;
constexpr uint32 k_mac_address_high = 0x0040;
constexpr uint32 k_mac_address_low = 0x0044;
constexpr uint32 k_mac_rgsmii_status = 0x00d8;
constexpr uint32 k_mac_mmc_rx_mask = 0x010c;
constexpr uint32 k_mac_mmc_tx_mask = 0x0110;
constexpr uint32 k_mac_mmc_ipc_mask = 0x0200;

constexpr uint32 k_dma_base = 0x1000;
constexpr uint32 k_dma_bus_mode = 0x0000;
constexpr uint32 k_dma_tx_poll = 0x0004;
constexpr uint32 k_dma_rx_poll = 0x0008;
constexpr uint32 k_dma_rx_base = 0x000c;
constexpr uint32 k_dma_tx_base = 0x0010;
constexpr uint32 k_dma_status = 0x0014;
constexpr uint32 k_dma_control = 0x0018;
constexpr uint32 k_dma_interrupt = 0x001c;
constexpr uint32 k_dma_axi_bus_mode = 0x0028;

constexpr uint32 k_mac_rx = 1U << 2;
constexpr uint32 k_mac_tx = 1U << 3;
constexpr uint32 k_mac_deferral_check = 1U << 4;
constexpr uint32 k_mac_backoff_limit = 3U << 5;
constexpr uint32 k_mac_pad_crc_strip = 1U << 7;
constexpr uint32 k_mac_retry_disable = 1U << 9;
constexpr uint32 k_mac_duplex = 1U << 11;
constexpr uint32 k_mac_loopback = 1U << 12;
constexpr uint32 k_mac_rx_own = 1U << 13;
constexpr uint32 k_mac_speed_100 = 1U << 14;
constexpr uint32 k_mac_port_select = 1U << 15;
constexpr uint32 k_mac_jumbo = 1U << 20;
constexpr uint32 k_mac_frame_burst = 1U << 21;
constexpr uint32 k_mac_jabber = 1U << 22;
constexpr uint32 k_mac_watchdog = 1U << 23;
constexpr uint32 k_mac_tx_config = 1U << 24;
constexpr uint32 k_mac_filter = 1U << 31;
constexpr uint32 k_mac_pause_time_mask = 0xffff0000U;

constexpr uint32 k_link_full_duplex = 1U << 0;
constexpr uint32 k_link_speed_100 = 1U << 1;
constexpr uint32 k_link_speed_1000 = 1U << 2;
constexpr uint32 k_link_speed_mask = k_link_speed_100 | k_link_speed_1000;
constexpr uint32 k_link_up = 1U << 3;

constexpr uint32 k_dma_reset = 1U << 0;
constexpr uint32 k_dma_burst_length_32 = 1U << 13;
constexpr uint32 k_dma_burst_length_x8 = 1U << 24;
constexpr uint32 k_dma_mixed_burst = 1U << 26;
constexpr uint32 k_dma_rx_start = 1U << 1;
constexpr uint32 k_dma_tx_second_frame = 1U << 2;
constexpr uint32 k_dma_tx_start = 1U << 13;
constexpr uint32 k_dma_store_forward = 0x02200000U;
constexpr uint32 k_dma_rx_stopped = 1U << 8;
constexpr uint32 k_dma_tx_stopped = 1U << 1;
constexpr uint32 k_dma_fatal_bus_error = 1U << 13;

constexpr uint32 k_descriptor_size_mask = 0x1fff;
constexpr uint32 k_rx_end_of_ring = 1U << 15;
constexpr uint32 k_tx_end_of_ring = 1U << 21;
constexpr uint32 k_tx_first = 1U << 28;
constexpr uint32 k_tx_last = 1U << 29;
constexpr uint32 k_rx_last = 1U << 8;
constexpr uint32 k_rx_first = 1U << 9;
constexpr uint32 k_descriptor_error = 1U << 15;
constexpr uint32 k_frame_length_mask = 0x3fff0000U;
constexpr uint32 k_frame_length_shift = 16;
constexpr uint32 k_owned_by_dma = 1U << 31;

struct DmaDescriptor
{
    uint32 status;
    uint32 length;
    uint32 buffer1;
    uint32 buffer2;
};

struct alignas(64) DescriptorRings
{
    DmaDescriptor tx[k_ring_size];
    DmaDescriptor rx[k_ring_size];
};

struct alignas(64) DmaBuffers
{
    uint8 tx[k_ring_size][k_buffer_size];
    uint8 rx[k_ring_size][k_buffer_size];
};

alignas(64) DescriptorRings g_ring_storage
    __attribute__((section(".dma_uncached")));
alignas(64) DmaBuffers g_buffer_storage
    __attribute__((section(".dma_uncached")));
SpinLock g_lock;
bool g_initialized = false;
uint32 g_tx_next = 0;
uint32 g_rx_next = 0;
uint8 g_mac[6]{};

volatile uint32 *mac_register(uint32 offset)
{
    return reinterpret_cast<volatile uint32 *>(
        board::mmio_address(board::k_gmac0_physical) + offset);
}

volatile uint32 *dma_register(uint32 offset)
{
    return reinterpret_cast<volatile uint32 *>(
        board::mmio_address(board::k_gmac0_physical) + k_dma_base + offset);
}

uint32 mac_read(uint32 offset)
{
    return *mac_register(offset);
}

void mac_write(uint32 offset, uint32 value)
{
    *mac_register(offset) = value;
}

uint32 dma_read(uint32 offset)
{
    return *dma_register(offset);
}

void dma_write(uint32 offset, uint32 value)
{
    *dma_register(offset) = value;
}

void dma_barrier()
{
    asm volatile("dbar 0" ::: "memory");
}

template <typename T>
T *dma_alias(T *pointer)
{
    return reinterpret_cast<T *>(board::mmio_address(reinterpret_cast<uint64>(pointer)));
}

uint32 dma_address(const void *pointer)
{
    const uint64 physical = board::physical_address(reinterpret_cast<uint64>(pointer));
    return physical <= 0xffffffffULL ? static_cast<uint32>(physical) : 0;
}

bool dma_range_is_32bit(const void *pointer, uint64 size)
{
    const uint64 physical = board::physical_address(reinterpret_cast<uint64>(pointer));
    return size != 0 && physical <= 0xffffffffULL &&
           size - 1 <= 0xffffffffULL - physical;
}

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

bool read_firmware_mac(uint8 mac[6])
{
    const uint32 low = mac_read(k_mac_address_low);
    const uint32 high = mac_read(k_mac_address_high);
    mac[0] = static_cast<uint8>(low);
    mac[1] = static_cast<uint8>(low >> 8);
    mac[2] = static_cast<uint8>(low >> 16);
    mac[3] = static_cast<uint8>(low >> 24);
    mac[4] = static_cast<uint8>(high);
    mac[5] = static_cast<uint8>(high >> 8);
    return valid_mac(mac);
}

uint32 next_index(uint32 index)
{
    return (index + 1) % k_ring_size;
}

void configure_link(uint32 link)
{
    const uint32 old_config = mac_read(k_mac_config);
    uint32 config = old_config & ~(k_mac_port_select | k_mac_speed_100 | k_mac_duplex);
    if ((link & k_link_full_duplex) != 0)
    {
        config |= k_mac_duplex;
    }
    switch (link & k_link_speed_mask)
    {
    case k_link_speed_1000:
        break;
    case k_link_speed_100:
        config |= k_mac_port_select | k_mac_speed_100;
        break;
    default:
        config |= k_mac_port_select;
        break;
    }
    if (config != old_config)
    {
        mac_write(k_mac_config, config);
    }
}

void initialize_descriptors()
{
    auto *rings = dma_alias(&g_ring_storage);
    auto *buffers = dma_alias(&g_buffer_storage);
    memset(rings, 0, sizeof(*rings));
    memset(buffers, 0, sizeof(*buffers));

    for (uint32 index = 0; index < k_ring_size; ++index)
    {
        rings->tx[index].status = index == k_ring_size - 1 ? k_tx_end_of_ring : 0;
        rings->rx[index].length = k_buffer_size |
                                  (index == k_ring_size - 1 ? k_rx_end_of_ring : 0);
        rings->rx[index].buffer1 = dma_address(&g_buffer_storage.rx[index][0]);
        rings->rx[index].status = k_owned_by_dma;
    }
    dma_barrier();
}

bool reset_dma()
{
    dma_write(k_dma_bus_mode, k_dma_reset);
    const uint64 start = rdtime();
    const uint64 timeout_cycles = tmm::qemu_fre_cal_cycles(k_dma_reset_timeout_us);
    while (rdtime() - start < timeout_cycles)
    {
        if ((dma_read(k_dma_bus_mode) & k_dma_reset) == 0)
        {
            return true;
        }
        asm volatile("nop");
    }
    return false;
}

void clear_pending_status()
{
    mac_write(k_mac_mmc_tx_mask, 0xffffffffU);
    mac_write(k_mac_mmc_rx_mask, 0xffffffffU);
    mac_write(k_mac_mmc_ipc_mask, 0xffffffffU);
    const uint32 status = dma_read(k_dma_status);
    dma_write(k_dma_status, status);
}

void rearm_rx(DmaDescriptor &descriptor, uint32 index)
{
    descriptor.status = 0;
    descriptor.length = k_buffer_size |
                        (index == k_ring_size - 1 ? k_rx_end_of_ring : 0);
    descriptor.buffer1 = dma_address(&g_buffer_storage.rx[index][0]);
    descriptor.buffer2 = 0;
    dma_barrier();
    descriptor.status = k_owned_by_dma;
    dma_barrier();
    dma_write(k_dma_rx_poll, 0);
}
} // namespace

bool initialize()
{
    g_lock.init("ls2k gmac");
    g_initialized = false;
    g_tx_next = 0;
    g_rx_next = 0;
    boardPrintfInfo("[gmac] input: physical=0x%lx mmio=0x%lx irq=%u "
                    "version=0x%x config=0x%x link=0x%x dma-bus=0x%x\n",
                    board::k_gmac0_physical,
                    board::mmio_address(board::k_gmac0_physical),
                    board::k_gmac0_interrupt, mac_read(k_mac_version),
                    mac_read(k_mac_config), mac_read(k_mac_rgsmii_status),
                    dma_read(k_dma_bus_mode));
    // MAC 必须来自板级 DTB 或固件已编程寄存器。固定回退地址会让多块板产生
    // 二层冲突；两处都没有有效地址时宁可禁用网卡并给出明确诊断。
    const bool mac_from_dtb =
        DtbManager::get_mac_address(board::k_gmac0_physical, g_mac);
    if (!mac_from_dtb && !read_firmware_mac(g_mac))
    {
        boardPrintfError("[gmac] no valid MAC in DTB or firmware registers\n");
        return false;
    }

    uint32 config = mac_read(k_mac_config);
    config &= ~(k_mac_rx | k_mac_tx);
    mac_write(k_mac_config, config);
    dma_write(k_dma_control, dma_read(k_dma_control) & ~(k_dma_rx_start | k_dma_tx_start));
    dma_write(k_dma_interrupt, 0);
    if (!reset_dma())
    {
        boardPrintfError("[gmac] DMA reset timeout: bus-mode=0x%x status=0x%x\n",
                         dma_read(k_dma_bus_mode), dma_read(k_dma_status));
        return false;
    }

    if (!dma_range_is_32bit(&g_ring_storage, sizeof(g_ring_storage)) ||
        !dma_range_is_32bit(&g_buffer_storage, sizeof(g_buffer_storage)))
    {
        boardPrintfError("[gmac] descriptor/buffer storage is outside the 32-bit DMA window\n");
        return false;
    }
    const uint32 tx_ring = dma_address(&g_ring_storage.tx[0]);
    const uint32 rx_ring = dma_address(&g_ring_storage.rx[0]);
    initialize_descriptors();

    dma_write(k_dma_bus_mode,
              k_dma_mixed_burst | k_dma_burst_length_x8 | k_dma_burst_length_32);
    dma_write(k_dma_control, k_dma_store_forward | k_dma_tx_second_frame);
    dma_write(k_dma_axi_bus_mode, 0xffU | (0x77U << 16));
    dma_write(k_dma_tx_base, tx_ring);
    dma_write(k_dma_rx_base, rx_ring);

    config = mac_read(k_mac_config) | k_mac_tx_config;
    config &= ~(k_mac_watchdog | k_mac_jabber | k_mac_frame_burst | k_mac_jumbo |
                k_mac_rx_own | k_mac_loopback | k_mac_retry_disable |
                k_mac_pad_crc_strip | k_mac_deferral_check | k_mac_backoff_limit);
    mac_write(k_mac_config, config | k_mac_duplex);
    mac_write(k_mac_frame_filter, k_mac_filter);
    mac_write(k_mac_flow_control, k_mac_pause_time_mask);
    mac_write(k_mac_address_high,
              (static_cast<uint32>(g_mac[5]) << 8) | g_mac[4]);
    mac_write(k_mac_address_low,
              (static_cast<uint32>(g_mac[3]) << 24) |
                  (static_cast<uint32>(g_mac[2]) << 16) |
                  (static_cast<uint32>(g_mac[1]) << 8) | g_mac[0]);

    configure_link(mac_read(k_mac_rgsmii_status));
    clear_pending_status();
    mac_write(k_mac_interrupt_mask, 0xffffffffU);
    mac_write(k_mac_config, mac_read(k_mac_config) | k_mac_rx | k_mac_tx);
    dma_write(k_dma_control, dma_read(k_dma_control) | k_dma_rx_start | k_dma_tx_start);
    dma_barrier();
    dma_write(k_dma_rx_poll, 0);

    g_initialized = true;
    const uint32 link = mac_read(k_mac_rgsmii_status);
    boardPrintfInfo("[gmac] output: ready mac=%02x:%02x:%02x:%02x:%02x:%02x "
                    "source=%s link=%s raw-link=0x%x tx-ring=0x%x rx-ring=0x%x\n",
                    g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
                    mac_from_dtb ? "DTB" : "firmware-registers",
                    (link & k_link_up) != 0 ? "up" : "down", link,
                    tx_ring, rx_ring);
    return true;
}

int send(const void *data, uint32 length)
{
    if (!g_initialized || data == nullptr || length == 0 || length > k_buffer_size)
    {
        return -1;
    }

    g_lock.acquire();
    if ((mac_read(k_mac_rgsmii_status) & k_link_up) == 0)
    {
        g_lock.release();
        return -1;
    }
    auto *rings = dma_alias(&g_ring_storage);
    auto *buffers = dma_alias(&g_buffer_storage);
    DmaDescriptor &descriptor = rings->tx[g_tx_next];
    if ((descriptor.status & k_owned_by_dma) != 0)
    {
        g_lock.release();
        return -1;
    }

    memmove(&buffers->tx[g_tx_next][0], data, length);
    descriptor.status = g_tx_next == k_ring_size - 1 ? k_tx_end_of_ring : 0;
    descriptor.length = length & k_descriptor_size_mask;
    descriptor.buffer1 = dma_address(&g_buffer_storage.tx[g_tx_next][0]);
    descriptor.buffer2 = 0;
    dma_barrier();
    descriptor.status = k_owned_by_dma | k_tx_first | k_tx_last |
                        (g_tx_next == k_ring_size - 1 ? k_tx_end_of_ring : 0);
    dma_barrier();
    g_tx_next = next_index(g_tx_next);
    dma_write(k_dma_tx_poll, 0);
    g_lock.release();
    return 0;
}

int receive(void *data, uint32 *length)
{
    if (!g_initialized || data == nullptr || length == nullptr)
    {
        return -1;
    }

    g_lock.acquire();
    auto *rings = dma_alias(&g_ring_storage);
    auto *buffers = dma_alias(&g_buffer_storage);
    DmaDescriptor &descriptor = rings->rx[g_rx_next];
    if ((descriptor.status & k_owned_by_dma) != 0)
    {
        *length = 0;
        g_lock.release();
        return 0;
    }
    dma_barrier();

    const uint32 status = descriptor.status;
    uint32 packet_length = (status & k_frame_length_mask) >> k_frame_length_shift;
    const bool valid = (status & k_descriptor_error) == 0 &&
                       (status & k_rx_first) != 0 && (status & k_rx_last) != 0;
    if (!valid)
    {
        packet_length = 0;
    }
    else if (packet_length >= k_ethernet_fcs_size)
    {
        // Synopsys GMAC 接收描述符的 frame length 包含线路上的 4 字节 FCS，
        // ONPS 只接收以太网头和载荷，不能把 FCS 当作协议数据继续上传。
        packet_length -= k_ethernet_fcs_size;
    }
    else
    {
        packet_length = 0;
    }
    if (packet_length > *length)
    {
        packet_length = *length;
    }
    if (packet_length != 0)
    {
        memmove(data, &buffers->rx[g_rx_next][0], packet_length);
    }
    rearm_rx(descriptor, g_rx_next);
    g_rx_next = next_index(g_rx_next);
    *length = packet_length;
    g_lock.release();
    return valid ? 0 : -1;
}

void poll()
{
    if (!g_initialized)
    {
        return;
    }
    g_lock.acquire();
    const uint32 status = dma_read(k_dma_status);
    if (status != 0 && status != 0xffffffffU)
    {
        dma_write(k_dma_status, status);
        if ((status & k_dma_rx_stopped) != 0)
        {
            dma_write(k_dma_control, dma_read(k_dma_control) | k_dma_rx_start);
            dma_write(k_dma_rx_poll, 0);
        }
        if ((status & k_dma_tx_stopped) != 0)
        {
            dma_write(k_dma_control, dma_read(k_dma_control) | k_dma_tx_start);
            dma_write(k_dma_tx_poll, 0);
        }
        if ((status & k_dma_fatal_bus_error) != 0)
        {
            boardPrintfError("[gmac] fatal DMA bus error: status=0x%x\n", status);
        }
    }
    configure_link(mac_read(k_mac_rgsmii_status));
    g_lock.release();
}

void get_mac(uint8 mac[6])
{
    if (mac != nullptr)
    {
        memmove(mac, g_mac, sizeof(g_mac));
    }
}

void debug_status()
{
    boardPrintf("[gmac] status: mac-config=0x%x link=0x%x dma-status=0x%x "
                "dma-control=0x%x\n",
                mac_read(k_mac_config), mac_read(k_mac_rgsmii_status),
                dma_read(k_dma_status), dma_read(k_dma_control));
}
} // namespace loongarch::ls2k1000::gmac

#endif

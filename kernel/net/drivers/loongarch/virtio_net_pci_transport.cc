#include "net/drivers/virtio_net_transport.hh"

#include "devs/virtio/pci.hh"
#include "hal/loongarch/platform_board.hh"
#include "libs/printer.hh"
#include "libs/string.hh"
#include "platform/pci.hh"

namespace net::virtio_transport
{
namespace
{
constexpr uint16 k_vendor_id = 0x1af4;
constexpr uint16 k_device_id = 0x1000;
constexpr uint8 k_status_acknowledge = 1;
constexpr uint8 k_status_driver = 2;
constexpr uint8 k_status_driver_ok = 4;
constexpr uint8 k_status_features_ok = 8;

virtio_pci_hw_t g_hardware;
uint8 g_status;
bool g_prepared;

bool configure_queue(uint16 index, const QueueMemory &memory,
                     uint16 requested_size)
{
    const uint16 maximum_size = virtio_pci_get_queue_size(&g_hardware, index);
    if (maximum_size < requested_size)
    {
        printf("virtio net: pci queue %u too small, max=%u\n",
               index, maximum_size);
        return false;
    }
    if (virtio_pci_get_queue_enable(&g_hardware, index) != 0)
    {
        printf("virtio net: pci queue %u is already enabled\n", index);
        return false;
    }

    virtio_pci_set_queue_size(&g_hardware, index, requested_size);
    // 公共网卡层已经分别翻译三段 ring 的 DMA 地址；PCI 协议层只负责
    // 写入寄存器，不能再次把 CPU 指针按某一种地址模型解释。
    virtio_pci_set_queue_addresses(&g_hardware, index,
                                   memory.descriptor_dma,
                                   memory.available_dma,
                                   memory.used_dma);
    virtio_pci_set_queue_enable(&g_hardware, index);
    return true;
}
} // namespace

bool prepare(const PrepareArgs &args, uint64 *negotiated_features)
{
    if (negotiated_features == nullptr || args.mac_address == nullptr ||
        args.mac_address_length == 0 || args.queue_size == 0)
    {
        return false;
    }

    memset(&g_hardware, 0, sizeof(g_hardware));
    g_status = 0;
    g_prepared = false;

    platform::pci::FunctionAddress address{};
    if (!platform::pci::find_device(k_vendor_id, k_device_id, 0, address))
    {
        printf("virtio net: pci device not found\n");
        return false;
    }

    platform::pci::enable_and_allocate_bars(address);
    if (virtio_pci_read_caps(&g_hardware, address) != 0)
    {
        printf("virtio net: read pci caps failed\n");
        return false;
    }

    if (!virtio_pci_reset(&g_hardware))
    {
        printf("virtio net: pci device reset timeout\n");
        return false;
    }
    g_status = k_status_acknowledge;
    virtio_pci_set_status(&g_hardware, g_status);
    g_status |= k_status_driver;
    virtio_pci_set_status(&g_hardware, g_status);

    const uint64 device_features = virtio_pci_get_device_features(&g_hardware);
    const uint64 required_features = 1ULL << k_virtio_feature_version_1;
    if ((device_features & required_features) == 0)
    {
        printf("virtio net: pci VERSION_1 feature missing\n");
        return false;
    }

    // 公共收发层目前只理解普通 split ring 和可选设备 MAC；transport
    // 必须补上 modern PCI 必需的 VERSION_1，且不能接受 packed ring 等
    // args 未明确支持的设备能力。
    const uint64 supported_features =
        required_features | (1ULL << k_feature_mac);
    const uint64 features =
        device_features & supported_features &
        (args.driver_features | required_features);
    virtio_pci_set_driver_features(&g_hardware, features);
    g_status |= k_status_features_ok;
    virtio_pci_set_status(&g_hardware, g_status);
    if ((virtio_pci_get_status(&g_hardware) & k_status_features_ok) == 0)
    {
        printf("virtio net: pci FEATURES_OK was rejected\n");
        return false;
    }

    if (!configure_queue(k_receive_queue_index, args.receive_queue,
                         args.queue_size) ||
        !configure_queue(k_transmit_queue_index, args.transmit_queue,
                         args.queue_size))
    {
        return false;
    }

    if ((features & (1ULL << k_feature_mac)) != 0)
    {
        // MAC 是 device_cfg 的前 6 字节；即使 capability 没越过
        // BAR，过短的子区间仍是损坏配置，不能继续读。
        if (g_hardware.device_cfg == nullptr ||
            g_hardware.device_cfg_length < args.mac_address_length)
        {
            printf("virtio net: pci device config is too short for MAC\n");
            return false;
        }
        if (!virtio_pci_read_device_config_bytes(
                &g_hardware, 0, args.mac_address,
                args.mac_address_length))
        {
            printf("virtio net: pci MAC config did not stabilize\n");
            return false;
        }
    }

    *negotiated_features = features;
    g_prepared = true;
    return true;
}

void activate()
{
    if (!g_prepared)
    {
        panic("virtio net: activate pci transport before prepare");
    }
    g_status |= k_status_driver_ok;
    virtio_pci_set_status(&g_hardware, g_status);
}

void notify_queue(uint16 queue_index)
{
    virtio_pci_set_queue_notify(&g_hardware, queue_index);
}

void acknowledge_interrupt()
{
    if (g_prepared)
    {
        virtio_pci_clear_isr(&g_hardware);
    }
}

uint32 interrupt_source()
{
    // QEMU LoongArch virt 的 PCI INTx 共享 PCH source 32；公共 IRQ registry
    // 会在同一 source 上顺序调用块设备与网卡各自的确认函数。
    return loongarch::board::k_pci_intx_interrupt;
}
} // namespace net::virtio_transport

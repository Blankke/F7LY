#include "fs/drivers/virtio_blk.hh"
#include "fs/drivers/virtio_blk_device.hh"
#include "fs/drivers/virtio_blk_transport.hh"
#include "global_operator.hh"
#include "hal/arch.hh"
#include "hal/irq.hh"
#include "hal/loongarch/platform_board.hh"
#include "mem/virtual_memory_manager.hh"
#include "param.h"
#include "platform/pci.hh"
#include "printer.hh"
#include "devs/virtio/pci.hh"

#ifdef LOONGARCH
namespace
{
    constexpr uint16 k_virtio_blk_vendor = 0x1af4;
    constexpr uint16 k_virtio_blk_device = 0x1001;

    constexpr uint8 k_status_acknowledge = 1;
    constexpr uint8 k_status_driver = 2;
    constexpr uint8 k_status_driver_ok = 4;
    constexpr uint8 k_status_features_ok = 8;

    class PciVirtioBlkTransport final : public virtio_blk::VirtioBlkTransport
    {
    public:
        PciVirtioBlkTransport() = default;

        uint64 dma_addr(const void *ptr) const override
        {
            const uint64 va = reinterpret_cast<uint64>(ptr);

            // LoongArch 的 PMM/HMM 返回 DMWIN 直映地址（0x9xxx...）。这类
            // 地址不经过内核页表，直接去掉 DMWIN 高位即可得到 DMA 物理地址；
            // 只有普通内核虚拟地址才需要通过 k_pagetable 做页表翻译。
            // 动态高端内存启用后，virtio 队列和块缓冲区主要来自 DMWIN，
            // 继续无条件 walk 会把合法的直映地址误判成未映射。
            uint64 pa = 0;
            if ((va & VIRT_DMWIN_MASK) == DMWIN_MASK)
            {
                pa = VIRT2PHY(va);
            }
            else
            {
                pa = mem::k_pagetable.kwalk_addr(va);
            }
            if (pa == 0)
            {
                panic("loongarch virtio blk: dma addr translate failed");
            }
            return pa;
        }

        void notify_queue(uint16 queue_index) override
        {
            virtio_pci_set_queue_notify(&hw_, queue_index);
        }

        void queue_memory_barrier() override
        {
            dsb();
        }

        void ack_interrupt() override
        {
            virtio_pci_clear_isr(&hw_);
            dsb();
        }

        bool polling_wait() const override
        {
            return true;
        }

        virtio_pci_hw_t &hardware()
        {
            return hw_;
        }

    private:
        virtio_pci_hw_t hw_ = {};
    };

    alignas(PciVirtioBlkTransport) unsigned char g_primary_transport_storage[sizeof(PciVirtioBlkTransport)];
    alignas(PciVirtioBlkTransport) unsigned char g_secondary_transport_storage[sizeof(PciVirtioBlkTransport)];
    alignas(virtio_blk::VirtioBlkDevice) unsigned char g_primary_device_storage[sizeof(virtio_blk::VirtioBlkDevice)];
    alignas(virtio_blk::VirtioBlkDevice) unsigned char g_secondary_device_storage[sizeof(virtio_blk::VirtioBlkDevice)];

    PciVirtioBlkTransport *g_primary_transport = nullptr;
    PciVirtioBlkTransport *g_secondary_transport = nullptr;
    virtio_blk::VirtioBlkDevice *g_primary_device = nullptr;
    virtio_blk::VirtioBlkDevice *g_secondary_device = nullptr;

    void virtio_block_irq_handler(void *context)
    {
        static_cast<virtio_blk::VirtioBlkDevice *>(context)->handle_interrupt();
    }

    void register_block_irq(virtio_blk::VirtioBlkDevice &device,
                            const char *name)
    {
        const uint32 source = loongarch::board::k_pci_intx_interrupt;
        if (!hal::irq::register_handler(
                source, virtio_block_irq_handler, &device, name))
        {
            panic("failed to register virtio block IRQ source %u", source);
        }
    }

    PciVirtioBlkTransport &ensure_pci_transport(PciVirtioBlkTransport *&transport_ptr,
                                                unsigned char *storage)
    {
        if (transport_ptr == nullptr)
        {
            transport_ptr = new (storage) PciVirtioBlkTransport();
        }
        return *transport_ptr;
    }

    virtio_blk::VirtioBlkDevice &ensure_device(virtio_blk::VirtioBlkDevice *&device_ptr,
                                               unsigned char *storage)
    {
        if (device_ptr == nullptr)
        {
            device_ptr = new (storage) virtio_blk::VirtioBlkDevice();
        }
        return *device_ptr;
    }

    void init_pci_disk(uint32 pci_instance,
                       PciVirtioBlkTransport &transport,
                       virtio_blk::VirtioBlkDevice &blk_device,
                       const char *lock_name,
                       int owner_token)
    {
        platform::pci::FunctionAddress address{};
        if (!platform::pci::find_device(k_virtio_blk_vendor,
                                        k_virtio_blk_device,
                                        pci_instance,
                                        address))
        {
            panic("loongarch virtio blk: PCI instance %u not found",
                  pci_instance);
        }

        platform::pci::enable_and_allocate_bars(address);
        if (virtio_pci_read_caps(&transport.hardware(), address) != 0)
        {
            panic("loongarch virtio blk: read pci caps failed");
        }
        // device_cfg 对 VirtIO PCI 传输本身是可选 capability，但块设备
        // 必须从其首个 64 位字段读取容量，因此由块驱动声明自己的要求。
        if (transport.hardware().device_cfg == nullptr ||
            transport.hardware().device_cfg_length < sizeof(uint64))
        {
            panic("loongarch virtio blk: device config missing capacity");
        }

        blk_device.initialize({lock_name, owner_token, &transport});

        if (!virtio_pci_reset(&transport.hardware()))
        {
            panic("loongarch virtio blk: device reset timeout");
        }

        uint8 status = 0;
        status |= k_status_acknowledge;
        virtio_pci_set_status(&transport.hardware(), status);

        status |= k_status_driver;
        virtio_pci_set_status(&transport.hardware(), status);

        const uint64 device_features =
            virtio_pci_get_device_features(&transport.hardware());
        const uint64 required_features =
            1ULL << k_virtio_feature_version_1;
        if ((device_features & required_features) == 0)
        {
            panic("loongarch virtio blk: VERSION_1 feature missing");
        }

        // 当前块队列只实现基础 split ring，不支持 packed ring、indirect
        // descriptor、event idx 或其它可选语义，因此只能回写明确支持的
        // VERSION_1。把设备所有 feature 原样回写会错误承诺未实现能力。
        virtio_pci_set_driver_features(&transport.hardware(),
                                       required_features);

        status |= k_status_features_ok;
        virtio_pci_set_status(&transport.hardware(), status);

        status = virtio_pci_get_status(&transport.hardware());
        if ((status & k_status_features_ok) == 0)
        {
            panic("loongarch virtio blk: FEATURES_OK unset");
        }

        if (virtio_pci_get_queue_enable(&transport.hardware(), 0))
        {
            panic("loongarch virtio blk: queue should not be ready");
        }

        uint32 max = virtio_pci_get_queue_size(&transport.hardware(), 0);
        if (max == 0)
        {
            panic("loongarch virtio blk: queue 0 missing");
        }
        if (max < virtio_blk::k_queue_size)
        {
            panic("loongarch virtio blk: queue too short");
        }

        virtio_pci_set_queue_size(&transport.hardware(), 0, virtio_blk::k_queue_size);
        virtio_pci_set_queue_addresses(
            &transport.hardware(), 0,
            transport.dma_addr(blk_device.desc_area()),
            transport.dma_addr(blk_device.avail_area()),
            transport.dma_addr(blk_device.used_area()));
        virtio_pci_set_queue_enable(&transport.hardware(), 0);

        status |= k_status_driver_ok;
        virtio_pci_set_status(&transport.hardware(), status);
    }
} // namespace

void virtio_disk_init()
{
    PciVirtioBlkTransport &transport = ensure_pci_transport(g_primary_transport, g_primary_transport_storage);
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_primary_device, g_primary_device_storage);
    init_pci_disk(0, transport, device, "virtio_disk_lock", 1);
    register_block_irq(device, "virtio-blk0");
}

void virtio_disk_init2(void)
{
    PciVirtioBlkTransport &transport = ensure_pci_transport(g_secondary_transport, g_secondary_transport_storage);
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_secondary_device, g_secondary_device_storage);
    init_pci_disk(1, transport, device, "virtio_disk2_lock", 2);
    register_block_irq(device, "virtio-blk1");
}

void virtio_disk_rw(struct buf *b, int write)
{
    g_primary_device->submit_and_wait(b, write);
}

void virtio_disk_rw2(struct buf *b, int write)
{
    g_secondary_device->submit_and_wait(b, write);
}

int virtio_disk_rw_sectors(int dev, void *buf, uint64 start_sector, uint32 sector_count, int write)
{
    if (sector_count == 0)
    {
        return 0;
    }

    virtio_blk::VirtioBlkDevice *device = nullptr;
    if (dev == 0)
    {
        device = g_primary_device;
    }
    else if (dev == 1)
    {
        device = g_secondary_device;
    }
    else
    {
        return -1;
    }

    return device->submit_transfer_and_wait(buf, sector_count * BSIZE, start_sector, write != 0);
}

uint64 virtio_disk_capacity_bytes(int dev)
{
    PciVirtioBlkTransport *transport = nullptr;
    if (dev == 0)
    {
        transport = g_primary_transport;
    }
    else if (dev == 1)
    {
        transport = g_secondary_transport;
    }
    if (transport == nullptr || transport->hardware().device_cfg == nullptr ||
        transport->hardware().device_cfg_length < sizeof(uint64))
    {
        return 0;
    }

    // Virtio-blk 设备配置的首字段是以 512 字节 sector 计数的
    // capacity。只有 capability 实际覆盖完整 8 字节时才能读取。
    uint64 sectors = 0;
    if (!virtio_pci_read_device_config_u64(
            &transport->hardware(), 0, sectors))
    {
        panic("loongarch virtio blk: unstable capacity config");
    }
    if (sectors > ~uint64{0} / static_cast<uint64>(BSIZE))
    {
        panic("loongarch virtio blk: capacity overflow");
    }
    return sectors * static_cast<uint64>(BSIZE);
}

void virtio_disk_intr(void)
{
    g_primary_device->handle_interrupt();
}

void virtio_disk_intr2()
{
    g_secondary_device->handle_interrupt();
}

#endif

#include "types.hh"
#include "param.h"
#include "trap/loongarch/pci.h"
#include "printer.hh"
#include "virtio_pci.hh"
#include "proc_manager.hh"
#include "scheduler.hh"
#include "virtual_memory_manager.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/drivers/virtio_blk_device.hh"
#include "fs/drivers/virtio_blk_transport.hh"
#include "fs/drivers/loongarch/virtio_blk_pci_state.hh"
#include "global_operator.hh"

#ifdef LOONGARCH
unsigned char bus1;
unsigned char device1;
unsigned char function1;
unsigned char bus2;
unsigned char device2;
unsigned char function2;

uint64 pci_base1;
uint64 pci_base2;

namespace
{
    constexpr uint16 k_virtio_blk_vendor = 0x1af4;
    constexpr uint16 k_virtio_blk_device = 0x1001;

    constexpr uint8 k_status_acknowledge = 1;
    constexpr uint8 k_status_driver = 2;
    constexpr uint8 k_status_driver_ok = 4;
    constexpr uint8 k_status_features_ok = 8;

    constexpr uint32 k_feature_blk_ro = 5;
    constexpr uint32 k_feature_blk_scsi = 7;
    constexpr uint32 k_feature_blk_config_wce = 11;
    constexpr uint32 k_feature_blk_mq = 12;
    constexpr uint32 k_feature_any_layout = 27;
    constexpr uint32 k_feature_event_idx = 29;
    constexpr uint32 k_feature_indirect_desc = 28;

    class PciVirtioBlkTransport final : public virtio_blk::VirtioBlkTransport
    {
    public:
        PciVirtioBlkTransport() = default;

        uint64 dma_addr(const void *ptr) const override
        {
            uint64 pa = mem::k_pagetable.kwalk_addr(reinterpret_cast<uint64>(ptr));
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

        void prepare_used_check() override
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

    void pci_device_init(uint64 pci_base, unsigned char bus, unsigned char device, unsigned char function)
    {
        mem::k_vmm.pci_map(bus, device, function, nullptr);

        unsigned int val = pci_config_read16(pci_base + PCI_STATUS_COMMAND);
        val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_IO;
        pci_config_write16(pci_base + PCI_STATUS_COMMAND, val);

        uint64 off = (bus << 16) | (device << 11) | (function << 8);
        volatile uint32 *base = reinterpret_cast<volatile uint32 *>(PCIE0_ECAM + off);
        for (int i = 0; i < 6; ++i)
        {
            uint32 old = base[4 + i];
            if (old & 0x1)
            {
                continue;
            }
            if (old & 0x4)
            {
                base[4 + i] = 0xffffffff;
                base[4 + i + 1] = 0xffffffff;
                __sync_synchronize();

                uint64 sz = (static_cast<uint64>(base[4 + i + 1]) << 32) | base[4 + i];
                sz = ~(sz & 0xFFFFFFFFFFFFFFF0ULL) + 1;
                uint64 mem_addr = pci_alloc_mmio(sz);
                base[4 + i] = static_cast<uint32>(mem_addr);
                base[4 + i + 1] = static_cast<uint32>(mem_addr >> 32);
                __sync_synchronize();
                ++i;
            }
        }
    }

    void init_pci_disk(uint64 pci_base,
                       unsigned char bus,
                       unsigned char device,
                       unsigned char function,
                       PciVirtioBlkTransport &transport,
                       virtio_blk::VirtioBlkDevice &blk_device,
                       const char *lock_name,
                       int owner_token)
    {
        if (pci_base == 0)
        {
            panic("loongarch virtio blk: pci device not found");
        }

        pci_device_init(pci_base, bus, device, function);
        if (virtio_pci_read_caps(&transport.hardware(), pci_base, 0) != 0)
        {
            panic("loongarch virtio blk: read pci caps failed");
        }

        blk_device.initialize({lock_name, owner_token, &transport});

        virtio_pci_set_status(&transport.hardware(), 0);

        uint8 status = 0;
        status |= k_status_acknowledge;
        virtio_pci_set_status(&transport.hardware(), status);

        status |= k_status_driver;
        virtio_pci_set_status(&transport.hardware(), status);

        uint64 features = virtio_pci_get_device_features(&transport.hardware());
        features &= ~(1ULL << k_feature_blk_ro);
        features &= ~(1ULL << k_feature_blk_scsi);
        features &= ~(1ULL << k_feature_blk_config_wce);
        features &= ~(1ULL << k_feature_blk_mq);
        features &= ~(1ULL << k_feature_any_layout);
        features &= ~(1ULL << k_feature_event_idx);
        features &= ~(1ULL << k_feature_indirect_desc);
        virtio_pci_set_driver_features(&transport.hardware(), features);

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
        virtio_pci_set_queue_addr2(&transport.hardware(), 0,
                                   blk_device.desc_area(),
                                   blk_device.avail_area(),
                                   blk_device.used_area());
        virtio_pci_set_queue_enable(&transport.hardware(), 0);

        status |= k_status_driver_ok;
        virtio_pci_set_status(&transport.hardware(), status);
    }
} // namespace

void virtio_probe()
{
    pci_base1 = pci_device_probe(k_virtio_blk_vendor, k_virtio_blk_device);
    pci_base2 = pci_device_probe(k_virtio_blk_vendor, k_virtio_blk_device);
}

void virtio_disk_init()
{
    PciVirtioBlkTransport &transport = ensure_pci_transport(g_primary_transport, g_primary_transport_storage);
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_primary_device, g_primary_device_storage);
    init_pci_disk(pci_base1, bus1, device1, function1, transport, device, "virtio_disk_lock", 1);
}

void virtio_disk_init2(void)
{
    PciVirtioBlkTransport &transport = ensure_pci_transport(g_secondary_transport, g_secondary_transport_storage);
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_secondary_device, g_secondary_device_storage);
    init_pci_disk(pci_base2, bus2, device2, function2, transport, device, "virtio_disk2_lock", 2);
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

void virtio_disk_intr(void)
{
    g_primary_device->handle_interrupt();
}

void virtio_disk_intr2()
{
    g_secondary_device->handle_interrupt();
}

#endif

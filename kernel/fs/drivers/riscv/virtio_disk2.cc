//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//

#include "types.hh"
#include "platform.hh"

#include "param.h"
#include "mem/memlayout.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/drivers/virtio_blk_device.hh"
#include "fs/drivers/virtio_blk_transport.hh"
#include "global_operator.hh"
#include "virtual_memory_manager.hh"

#ifdef RISCV

namespace
{
    constexpr uint32 k_mmio_magic_value = 0x000;
    constexpr uint32 k_mmio_version = 0x004;
    constexpr uint32 k_mmio_device_id = 0x008;
    constexpr uint32 k_mmio_vendor_id = 0x00c;
    constexpr uint32 k_mmio_device_features = 0x010;
    constexpr uint32 k_mmio_driver_features = 0x020;
    constexpr uint32 k_mmio_guest_page_size = 0x028;
    constexpr uint32 k_mmio_queue_sel = 0x030;
    constexpr uint32 k_mmio_queue_num_max = 0x034;
    constexpr uint32 k_mmio_queue_num = 0x038;
    constexpr uint32 k_mmio_queue_pfn = 0x040;
    constexpr uint32 k_mmio_queue_notify = 0x050;
    constexpr uint32 k_mmio_interrupt_status = 0x060;
    constexpr uint32 k_mmio_interrupt_ack = 0x064;
    constexpr uint32 k_mmio_status = 0x070;

    constexpr uint32 k_status_acknowledge = 1;
    constexpr uint32 k_status_driver = 2;
    constexpr uint32 k_status_driver_ok = 4;
    constexpr uint32 k_status_features_ok = 8;

    constexpr uint32 k_feature_blk_ro = 5;
    constexpr uint32 k_feature_blk_scsi = 7;
    constexpr uint32 k_feature_blk_config_wce = 11;
    constexpr uint32 k_feature_blk_mq = 12;
    constexpr uint32 k_feature_any_layout = 27;
    constexpr uint32 k_feature_ring_indirect_desc = 28;
    constexpr uint32 k_feature_ring_event_idx = 29;

    inline volatile uint32 *mmio_reg(uintptr_t base, uint32 offset)
    {
        return reinterpret_cast<volatile uint32 *>(base + offset);
    }

    class MmioVirtioBlkTransport final : public virtio_blk::VirtioBlkTransport
    {
    public:
        explicit MmioVirtioBlkTransport(uintptr_t base)
            : base_(base)
        {
        }

        uint64 dma_addr(const void *ptr) const override
        {
            uint64 pa = mem::k_pagetable.kwalk_addr(reinterpret_cast<uint64>(ptr));
            if (pa == 0)
            {
                panic("riscv virtio blk: dma addr translate failed");
            }
            return pa;
        }

        void notify_queue(uint16) override
        {
            *mmio_reg(base_, k_mmio_queue_notify) = 0;
        }

        void prepare_used_check() override
        {
            __sync_synchronize();
        }

        void ack_interrupt() override
        {
            volatile uint32 *status = mmio_reg(base_, k_mmio_interrupt_status);
            *mmio_reg(base_, k_mmio_interrupt_ack) = *status & 0x3;
        }

        bool polling_wait() const override
        {
            return false;
        }

    private:
        uintptr_t base_;
    };

    alignas(MmioVirtioBlkTransport) unsigned char g_primary_transport_storage[sizeof(MmioVirtioBlkTransport)];
    alignas(MmioVirtioBlkTransport) unsigned char g_secondary_transport_storage[sizeof(MmioVirtioBlkTransport)];
    alignas(virtio_blk::VirtioBlkDevice) unsigned char g_primary_device_storage[sizeof(virtio_blk::VirtioBlkDevice)];
    alignas(virtio_blk::VirtioBlkDevice) unsigned char g_secondary_device_storage[sizeof(virtio_blk::VirtioBlkDevice)];

    MmioVirtioBlkTransport *g_primary_transport = nullptr;
    MmioVirtioBlkTransport *g_secondary_transport = nullptr;
    virtio_blk::VirtioBlkDevice *g_primary_device = nullptr;
    virtio_blk::VirtioBlkDevice *g_secondary_device = nullptr;

    MmioVirtioBlkTransport &ensure_mmio_transport(MmioVirtioBlkTransport *&transport_ptr,
                                                  unsigned char *storage,
                                                  uintptr_t base)
    {
        if (transport_ptr == nullptr)
        {
            transport_ptr = new (storage) MmioVirtioBlkTransport(base);
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

    void init_mmio_disk(uintptr_t base,
                        virtio_blk::VirtioBlkDevice &device,
                        MmioVirtioBlkTransport &transport,
                        const char *lock_name,
                        int owner_token,
                        const char *panic_name)
    {
        uint32 status = 0;
        device.initialize({lock_name, owner_token, &transport});

        if (*mmio_reg(base, k_mmio_magic_value) != 0x74726976 ||
            *mmio_reg(base, k_mmio_version) != 1 ||
            *mmio_reg(base, k_mmio_device_id) != 2 ||
            *mmio_reg(base, k_mmio_vendor_id) != 0x554d4551)
        {
            panic(panic_name);
        }

        status |= k_status_acknowledge;
        *mmio_reg(base, k_mmio_status) = status;

        status |= k_status_driver;
        *mmio_reg(base, k_mmio_status) = status;

        uint64 features = *mmio_reg(base, k_mmio_device_features);
        features &= ~(1u << k_feature_blk_ro);
        features &= ~(1u << k_feature_blk_scsi);
        features &= ~(1u << k_feature_blk_config_wce);
        features &= ~(1u << k_feature_blk_mq);
        features &= ~(1u << k_feature_any_layout);
        features &= ~(1u << k_feature_ring_event_idx);
        features &= ~(1u << k_feature_ring_indirect_desc);
        *mmio_reg(base, k_mmio_driver_features) = static_cast<uint32>(features);

        status |= k_status_features_ok;
        *mmio_reg(base, k_mmio_status) = status;

        status |= k_status_driver_ok;
        *mmio_reg(base, k_mmio_status) = status;

        *mmio_reg(base, k_mmio_guest_page_size) = PGSIZE;
        *mmio_reg(base, k_mmio_queue_sel) = 0;

        uint32 max = *mmio_reg(base, k_mmio_queue_num_max);
        if (max == 0)
        {
            panic("virtio disk has no queue 0");
        }
        if (max < virtio_blk::k_queue_size)
        {
            panic("virtio disk max queue too short");
        }

        *mmio_reg(base, k_mmio_queue_num) = virtio_blk::k_queue_size;
        *mmio_reg(base, k_mmio_queue_pfn) = static_cast<uint32>(device.pages_dma_base() >> PGSHIFT);
    }
} // namespace

void virtio_disk_init(void)
{
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_primary_device, g_primary_device_storage);
    MmioVirtioBlkTransport &transport = ensure_mmio_transport(g_primary_transport, g_primary_transport_storage, VIRTIO0);
    init_mmio_disk(VIRTIO0, device, transport, "virtio_disk_lock", 1, "could not find virtio disk");
}

void virtio_disk_init2(void)
{
    virtio_blk::VirtioBlkDevice &device = ensure_device(g_secondary_device, g_secondary_device_storage);
    MmioVirtioBlkTransport &transport = ensure_mmio_transport(g_secondary_transport, g_secondary_transport_storage, VIRTIO1);
    init_mmio_disk(VIRTIO1, device, transport, "virtio_disk2_lock", 2, "could not find virtio disk2");
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

void virtio_disk_intr()
{
    g_primary_device->handle_interrupt();
}

void virtio_disk_intr2()
{
    g_secondary_device->handle_interrupt();
}

#endif

#include "platform/block_backend.hh"

#include "fs/drivers/virtio_blk.hh"

namespace platform::block_backend
{
    bool initialize()
    {
        virtio_disk_init();
        return virtio_disk_capacity_bytes(0) != 0;
    }

    int read_write(int dev, void *buffer, uint64 start_sector,
                   uint32 sector_count, bool write)
    {
        return virtio_disk_rw_sectors(
            dev, buffer, start_sector, sector_count, write ? 1 : 0);
    }

    uint64 capacity_bytes(int dev)
    {
        return virtio_disk_capacity_bytes(dev);
    }

    const char *name()
    {
        return "loongarch-qemu-virtio-blk";
    }
} // namespace platform::block_backend

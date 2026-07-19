#include "fs/drivers/riscv/disk.hh"
#include "fs/drivers/riscv/sdcard.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/drivers/riscv/ramdisk.hh"
void disk_init(void)
{
#ifdef QEMU
    virtio_disk_init();
#elif defined(SDCARD)
    sd_init();
#else
    ramdisk_init();
#endif
}

void disk_rw(buf *buf, bool write)
{
#ifdef QEMU
    virtio_disk_rw(buf, write);
#elif defined(SDCARD)
    if (write)
    {
        sd_write((uint32 *)buf->data, 128, buf->blockno);
    }
    else
    {
        // printfOrange("disk_rw: read blockno %u\n", buf->blockno);
        sd_read((uint32 *)buf->data, 128, buf->blockno);
    }
#else
    if (write)
    {
        ramdisk_write(buf);
    }
    else
    {
        ramdisk_read(buf);
    }
#endif
}

int disk_rw_sectors(int dev, void *buf, uint64 start_sector,
                    uint32 sector_count, bool write)
{
#ifdef QEMU
    return virtio_disk_rw_sectors(dev, buf, start_sector, sector_count, write ? 1 : 0);
#else
    // VF2 当前只挂载一张由 SDIO1 驱动提供的主卡，所有逻辑设备都映射到它。
    if (dev != 0 || buf == nullptr || sector_count == 0 ||
        start_sector > 0x7fffffffULL || sector_count > 0x00ffffffU)
    {
        return 1;
    }

    int word_count = static_cast<int>(sector_count * 128U);
    return write
               ? static_cast<int>(sd_write(reinterpret_cast<uint32 *>(buf), word_count,
                                            static_cast<int>(start_sector)))
               : static_cast<int>(sd_read(reinterpret_cast<uint32 *>(buf), word_count,
                                           static_cast<int>(start_sector)));
#endif
}

void disk_intr(void)
{
#ifdef QEMU
    virtio_disk_intr();
#else
    printf("should not have disk intr");
// dmac_intr(DMAC_CHANNEL0);
#endif
}

#include "fs/drivers/riscv/disk.hh"

#include "fs/drivers/riscv/ramdisk.hh"
#include "fs/drivers/riscv/sdcard.hh"
#include "fs/drivers/virtio_blk.hh"
#include "printer.hh"

#ifdef VISIONFIVE2
namespace
{
constexpr int k_vf2_partition_dev_count = 4;
uint64 g_vf2_partition_offsets[k_vf2_partition_dev_count] = {};
} // namespace

void disk_set_partition_offset(int dev, uint64 start_sector)
{
    if (dev >= 0 && dev < k_vf2_partition_dev_count)
    {
        g_vf2_partition_offsets[dev] = start_sector;
    }
}
#endif

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
#elif defined(VISIONFIVE2)
    if (dev < 0 || dev >= k_vf2_partition_dev_count || buf == nullptr || sector_count == 0 ||
        start_sector > 0x7fffffffULL || sector_count > 0x00ffffffU)
    {
        printfRed("[disk] invalid rw dev=%d start=%lu cnt=%u write=%d buf=%p\n",
                  dev, start_sector, sector_count, write ? 1 : 0, buf);
        return 1;
    }

    uint64 logical_start_sector = start_sector;
    start_sector += g_vf2_partition_offsets[dev];
    if (write)
    {
        printfMagenta("[disk] write dev=%d logical=%lu physical=%lu cnt=%u offset=%lu\n",
                      dev, logical_start_sector, start_sector, sector_count,
                      g_vf2_partition_offsets[dev]);
    }

    int word_count = static_cast<int>(sector_count * 128U);
    return write
               ? static_cast<int>(sd_write(reinterpret_cast<uint32 *>(buf), word_count,
                                            static_cast<int>(start_sector)))
               : static_cast<int>(sd_read(reinterpret_cast<uint32 *>(buf), word_count,
                                           static_cast<int>(start_sector)));
#else
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

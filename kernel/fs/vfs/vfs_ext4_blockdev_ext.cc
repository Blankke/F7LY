#include "vfs_ext4_blockdev_ext.hh"

#include "types.hh"

#include "libs/clist.h"
#include "spinlock.hh"
#include "fs/vfs/fs.hh"
#include "devs/device_manager.hh"
#include "devs/block_device.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_blockdev.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/misc/queue.hh"
#include "libs/string.hh"
#include "mem/physical_memory_manager.hh"

namespace
{
    constexpr uint32 k_ext4_physical_block_size = 4096;
    constexpr uint32 k_ext4_sector_per_block = k_ext4_physical_block_size / BSIZE;
    constexpr uint32 k_ext4_dma_bounce_pages = 32;
    constexpr uint32 k_ext4_dma_bounce_bytes = k_ext4_dma_bounce_pages * PGSIZE;
    constexpr uint32 k_ext4_dma_bounce_block_capacity = k_ext4_dma_bounce_bytes / k_ext4_physical_block_size;
} // namespace

static int blockdev_lock(struct ext4_blockdev *bdev);

static int blockdev_unlock(struct ext4_blockdev *bdev);

static int blockdev_open(struct ext4_blockdev *bdev);

static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);

static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);

static int blockdev_rw_common(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt, bool write);

static int blockdev_close(struct ext4_blockdev *bdev);


[[maybe_unused]]static int bdevice_lock = 0;
//只有一个vbd
struct ext4_blockdev_iface biface;
struct vfs_ext4_blockdev bvbdev;

struct ext4_blockdev_iface biface2;
struct vfs_ext4_blockdev bvbdev2;

struct vfs_ext4_blockdev *vfs_ext4_blockdev_from_bd(struct ext4_blockdev *bdev) {
    if (bdev == NULL) {
        return NULL;
    }
    return container_of(bdev, struct vfs_ext4_blockdev, bd);
}

static int vfs_ext4_blockdev_init(struct vfs_ext4_blockdev *vbdev, int dev) {
    uint8_t *ph_bbuf = NULL;
    struct ext4_blockdev *bd = NULL;
    //TODO: 支持动态内存分配
    //struct ext4_blockdev_iface *iface = NULL;
    struct ext4_blockdev_iface *iface = &biface;

    if (vbdev) {
        vbdev->dev = dev;
        sem_init(&vbdev->io_sem, 1, const_cast<char *>("ext4_bdev_io"));
        vbdev->ph_bbuf_page_count = k_ext4_dma_bounce_pages;
        vbdev->ph_bbuf = reinterpret_cast<uint8 *>(mem::k_pmm.alloc_pages((int)vbdev->ph_bbuf_page_count));

        bd = &vbdev->bd;
        bd->bdif = iface;
        bd->part_offset = 0;

        bd->part_size = (uint64)(512ull * 8ull * 1024ull * 1024ull );
        ph_bbuf = vbdev->ph_bbuf;

        iface->lock = blockdev_lock;
        iface->unlock = blockdev_unlock;
        iface->open = blockdev_open;
        iface->bread = blockdev_read;
        iface->bwrite = blockdev_write;
        iface->close = blockdev_close;


        iface->ph_bsize = k_ext4_physical_block_size;

        iface->ph_bbuf = ph_bbuf;
        iface->ph_bcnt = bd->part_size / (uint64) bd->bdif->ph_bsize;
    }
    return EOK;
}

//For rootfs
static int vfs_ext4_blockdev_init2(struct vfs_ext4_blockdev *vbdev, int dev) {
    uint8_t *ph_bbuf = NULL;
    struct ext4_blockdev *bd = NULL;
    //TODO: 支持动态内存分配
    //struct ext4_blockdev_iface *iface = NULL;
    struct ext4_blockdev_iface *iface = &biface2;

    if (vbdev) {
        vbdev->dev = dev;
        sem_init(&vbdev->io_sem, 1, const_cast<char *>("ext4_bdev_io_root"));
        vbdev->ph_bbuf_page_count = k_ext4_dma_bounce_pages;
        vbdev->ph_bbuf = reinterpret_cast<uint8 *>(mem::k_pmm.alloc_pages((int)vbdev->ph_bbuf_page_count));

        bd = &vbdev->bd;
        bd->bdif = iface;
        bd->part_offset = 0;

        bd->part_size = 768 * 1024 * 1024;

        ph_bbuf = vbdev->ph_bbuf;

        iface->lock = blockdev_lock;
        iface->unlock = blockdev_unlock;
        iface->open = blockdev_open;
        iface->bread = blockdev_read2;
        iface->bwrite = blockdev_write2;
        iface->close = blockdev_close;


        iface->ph_bsize = k_ext4_physical_block_size;

        iface->ph_bbuf = ph_bbuf;
        iface->ph_bcnt = bd->part_size / (uint64) bd->bdif->ph_bsize;
    }
    return EOK;
}

//TODO：支持动态分配
struct vfs_ext4_blockdev *vfs_ext4_blockdev_create(int dev) {
    struct vfs_ext4_blockdev *vbdev = &bvbdev;
    if (vbdev == NULL) {
        return NULL;
    }
    int r = vfs_ext4_blockdev_init(vbdev, dev);
    if (r != EOK) {
        printf("vfs_ext4_blockdev_init failed: %d\n", r);
        return NULL;
    }
    r = ext4_device_register(&vbdev->bd, DEV_NAME);
    if (r != EOK) {
        printf("ext4_device_register failed: %d\n", r);
        return NULL;
    }
    return vbdev;
}

//TODO：支持动态分配
struct vfs_ext4_blockdev *vfs_ext4_blockdev_create2(int dev) {
    struct vfs_ext4_blockdev *vbdev = &bvbdev2;
    if (vbdev == NULL) {
        return NULL;
    }
    int r = vfs_ext4_blockdev_init2(vbdev, dev);
    if (r != EOK) {
        printf("vfs_ext4_blockdev_init failed: %d\n", r);
        return NULL;
    }
    r = ext4_device_register(&vbdev->bd, "root_fs");
    if (r != EOK) {
        printf("ext4_device_register failed: %d\n", r);
        return NULL;
    }
    return vbdev;
}

int vfs_ext4_blockdev_destroy(struct vfs_ext4_blockdev *vbdev) {
    if (vbdev == NULL)
        return -EINVAL;
    if (vbdev->ph_bbuf != nullptr)
    {
        mem::k_pmm.free_pages(vbdev->ph_bbuf);
        vbdev->ph_bbuf = nullptr;
        vbdev->ph_bbuf_page_count = 0;
    }
    return EOK;
}

static int blockdev_lock(struct ext4_blockdev *bdev) {
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == NULL) {
        return -EINVAL;
    }
    sem_p(&vbdev->io_sem);
    return EOK;
}

static int blockdev_unlock(struct ext4_blockdev *bdev) {
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == NULL) {
        return -EINVAL;
    }
    sem_v(&vbdev->io_sem);
    return EOK;
}

static int blockdev_open(struct ext4_blockdev *bdev) { return EOK; }

static int blockdev_close(struct ext4_blockdev *bdev) { return EOK; }

namespace
{
    int submit_sector_transfer(int dev, void *buf, uint64 start_sector, uint32 sector_count, bool write)
    {
        dev::VirtualDevice *vdev = dev::k_devm.get_device(dev);
        if (vdev != nullptr && vdev->type() == dev::DeviceType::dev_block)
        {
            auto *bd = static_cast<dev::BlockDevice *>(vdev);
            dev::BufferDescriptor desc = {
                .buf_addr = reinterpret_cast<uint64>(buf),
                .buf_size = sector_count * BSIZE,
            };
            if (write)
            {
                return bd->write_blocks(start_sector, sector_count, &desc, 1);
            }
            return bd->read_blocks(start_sector, sector_count, &desc, 1);
        }

        return virtio_disk_rw_sectors(dev, buf, start_sector, sector_count, write ? 1 : 0);
    }
} // namespace

static int blockdev_rw_common(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt, bool write) {
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == nullptr) {
        return EIO;
    }

    if (vbdev->ph_bbuf == nullptr || vbdev->ph_bbuf_page_count == 0) {
        return EIO;
    }

    uint8 *cursor = reinterpret_cast<uint8 *>(buf);
    uint64 sector_id = blk_id * k_ext4_sector_per_block;
    uint32 remaining_blocks = blk_cnt;

    while (remaining_blocks > 0) {
        uint32 chunk_blocks = remaining_blocks > k_ext4_dma_bounce_block_capacity
                                  ? k_ext4_dma_bounce_block_capacity
                                  : remaining_blocks;
        uint32 chunk_bytes = chunk_blocks * k_ext4_physical_block_size;
        uint32 chunk_sectors = chunk_blocks * k_ext4_sector_per_block;

        /*
         * 当前 virtio blk 公共队列仍以“单数据段 DMA”建模。
         * 为了跨页时也保证物理连续，我们统一用连续页 bounce buffer 承载一次 chunk，
         * 这样 ext4 能把多个 4KiB 块合并成一次真正的块层传输，而不是 4KiB 一次地同步提交。
         */
        if (write) {
            memmove(vbdev->ph_bbuf, cursor, chunk_bytes);
        }

        int rc = submit_sector_transfer(vbdev->dev,
                                        vbdev->ph_bbuf,
                                        sector_id,
                                        chunk_sectors,
                                        write);
        if (rc != 0) {
            return rc;
        }

        if (!write) {
            memmove(cursor, vbdev->ph_bbuf, chunk_bytes);
        }

        cursor += chunk_bytes;
        sector_id += chunk_sectors;
        remaining_blocks -= chunk_blocks;
    }

    return EOK;
}

static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    return blockdev_rw_common(bdev, buf, blk_id, blk_cnt, false);
}

static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt) {
	return blockdev_rw_common(bdev, const_cast<void *>(buf), blk_id, blk_cnt, true);
}

//For rootfs
 int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    return blockdev_rw_common(bdev, buf, blk_id, blk_cnt, false);
}

 int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    return blockdev_rw_common(bdev, const_cast<void *>(buf), blk_id, blk_cnt, true);
}













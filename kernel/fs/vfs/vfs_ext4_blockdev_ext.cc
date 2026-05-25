#include "vfs_ext4_blockdev_ext.hh"

#include "types.hh"

#include "devs/block_device.hh"
#include "devs/device_manager.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_blockdev.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "libs/string.hh"
#include "mem/physical_memory_manager.hh"

namespace
{
    constexpr uint32 k_ext4_physical_block_size = 4096;
    constexpr uint32 k_ext4_sector_per_block = k_ext4_physical_block_size / BSIZE;
    constexpr uint32 k_ext4_dma_bounce_pages = 32;
    constexpr uint32 k_ext4_dma_bounce_bytes = k_ext4_dma_bounce_pages * PGSIZE;
    constexpr uint32 k_ext4_dma_bounce_block_capacity =
        k_ext4_dma_bounce_bytes / k_ext4_physical_block_size;

    // 目前工程里仍然只有两块 ext4 设备入口：数据盘与 rootfs。
    // 接口对象和设备对象保持静态存储期，避免在挂载早期再引入额外堆分配。
    struct ext4_blockdev_iface biface;
    struct vfs_ext4_blockdev bvbdev;
    struct ext4_blockdev_iface biface2;
    struct vfs_ext4_blockdev bvbdev2;
} // namespace

static int blockdev_lock(struct ext4_blockdev *bdev);
static int blockdev_unlock(struct ext4_blockdev *bdev);
static int blockdev_open(struct ext4_blockdev *bdev);
static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);
static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);
int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);
int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);
static int blockdev_rw_common(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt, bool write);
static int blockdev_close(struct ext4_blockdev *bdev);

struct vfs_ext4_blockdev *vfs_ext4_blockdev_from_bd(struct ext4_blockdev *bdev)
{
    if (bdev == nullptr)
    {
        return nullptr;
    }
    return container_of(bdev, struct vfs_ext4_blockdev, bd);
}

static int vfs_ext4_blockdev_init_common(struct vfs_ext4_blockdev *vbdev,
                                         int dev,
                                         struct ext4_blockdev_iface *iface,
                                         uint64 part_size,
                                         int (*bread_fn)(struct ext4_blockdev *, void *, uint64_t, uint32_t),
                                         int (*bwrite_fn)(struct ext4_blockdev *, const void *, uint64_t, uint32_t),
                                         char *sem_name)
{
    if (vbdev == nullptr || iface == nullptr)
    {
        return -EINVAL;
    }

    if (vbdev->ph_bbuf != nullptr)
    {
        mem::k_pmm.free_pages(vbdev->ph_bbuf);
        vbdev->ph_bbuf = nullptr;
        vbdev->ph_bbuf_page_count = 0;
    }

    vbdev->dev = dev;
    sem_init(&vbdev->io_sem, 1, sem_name);
    vbdev->ph_bbuf_page_count = k_ext4_dma_bounce_pages;
    vbdev->ph_bbuf = reinterpret_cast<uint8 *>(mem::k_pmm.alloc_pages((int)vbdev->ph_bbuf_page_count));
    if (vbdev->ph_bbuf == nullptr)
    {
        vbdev->ph_bbuf_page_count = 0;
        return -ENOMEM;
    }

    struct ext4_blockdev *bd = &vbdev->bd;
    bd->bdif = iface;
    bd->part_offset = 0;
    bd->part_size = part_size;

    iface->lock = blockdev_lock;
    iface->unlock = blockdev_unlock;
    iface->open = blockdev_open;
    iface->bread = bread_fn;
    iface->bwrite = bwrite_fn;
    iface->close = blockdev_close;
    // lwext4 以 4KiB 物理块为单位组织缓存；这里和底层 512B sector 做一次映射。
    iface->ph_bsize = k_ext4_physical_block_size;
    iface->ph_bbuf = vbdev->ph_bbuf;
    iface->ph_bcnt = bd->part_size / (uint64)iface->ph_bsize;

    return EOK;
}

static int vfs_ext4_blockdev_init(struct vfs_ext4_blockdev *vbdev, int dev)
{
    return vfs_ext4_blockdev_init_common(vbdev,
                                         dev,
                                         &biface,
                                         512ull * 8ull * 1024ull * 1024ull,
                                         blockdev_read,
                                         blockdev_write,
                                         const_cast<char *>("ext4_bdev_io"));
}

static int vfs_ext4_blockdev_init2(struct vfs_ext4_blockdev *vbdev, int dev)
{
    return vfs_ext4_blockdev_init_common(vbdev,
                                         dev,
                                         &biface2,
                                         768ull * 1024ull * 1024ull,
                                         blockdev_read2,
                                         blockdev_write2,
                                         const_cast<char *>("ext4_bdev_io_root"));
}

struct vfs_ext4_blockdev *vfs_ext4_blockdev_create(int dev)
{
    struct vfs_ext4_blockdev *vbdev = &bvbdev;
    int r = vfs_ext4_blockdev_init(vbdev, dev);
    if (r != EOK)
    {
        printf("vfs_ext4_blockdev_init failed: %d\n", r);
        return nullptr;
    }

    r = ext4_device_register(&vbdev->bd, DEV_NAME);
    if (r != EOK)
    {
        printf("ext4_device_register failed: %d\n", r);
        vfs_ext4_blockdev_destroy(vbdev);
        return nullptr;
    }
    return vbdev;
}

struct vfs_ext4_blockdev *vfs_ext4_blockdev_create2(int dev)
{
    struct vfs_ext4_blockdev *vbdev = &bvbdev2;
    int r = vfs_ext4_blockdev_init2(vbdev, dev);
    if (r != EOK)
    {
        printf("vfs_ext4_blockdev_init failed: %d\n", r);
        return nullptr;
    }

    r = ext4_device_register(&vbdev->bd, "root_fs");
    if (r != EOK)
    {
        printf("ext4_device_register failed: %d\n", r);
        vfs_ext4_blockdev_destroy(vbdev);
        return nullptr;
    }
    return vbdev;
}

int vfs_ext4_blockdev_destroy(struct vfs_ext4_blockdev *vbdev)
{
    if (vbdev == nullptr)
    {
        return -EINVAL;
    }

    if (vbdev->bd.bdif != nullptr)
    {
        vbdev->bd.bdif->ph_bbuf = nullptr;
        vbdev->bd.bdif->ph_bcnt = 0;
    }

    if (vbdev->ph_bbuf != nullptr)
    {
        mem::k_pmm.free_pages(vbdev->ph_bbuf);
        vbdev->ph_bbuf = nullptr;
        vbdev->ph_bbuf_page_count = 0;
    }

    vbdev->bd.bdif = nullptr;
    return EOK;
}

static int blockdev_lock(struct ext4_blockdev *bdev)
{
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == nullptr)
    {
        return -EINVAL;
    }
    sem_p(&vbdev->io_sem);
    return EOK;
}

static int blockdev_unlock(struct ext4_blockdev *bdev)
{
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == nullptr)
    {
        return -EINVAL;
    }
    sem_v(&vbdev->io_sem);
    return EOK;
}

static int blockdev_open(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

static int blockdev_close(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

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

static int blockdev_rw_common(struct ext4_blockdev *bdev,
                              void *buf,
                              uint64_t blk_id,
                              uint32_t blk_cnt,
                              bool write)
{
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_from_bd(bdev);
    if (vbdev == nullptr || vbdev->ph_bbuf == nullptr || vbdev->ph_bbuf_page_count == 0)
    {
        return EIO;
    }

    uint8 *cursor = reinterpret_cast<uint8 *>(buf);
    uint64 sector_id = blk_id * k_ext4_sector_per_block;
    uint32 remaining_blocks = blk_cnt;

    while (remaining_blocks > 0)
    {
        uint32 chunk_blocks = remaining_blocks > k_ext4_dma_bounce_block_capacity
                                  ? k_ext4_dma_bounce_block_capacity
                                  : remaining_blocks;
        uint32 chunk_bytes = chunk_blocks * k_ext4_physical_block_size;
        uint32 chunk_sectors = chunk_blocks * k_ext4_sector_per_block;

        // 统一用连续物理页作为 bounce buffer，既满足当前 virtio 传输建模，
        // 也让 lwext4 能把多个 4KiB 块合并成一次真正的底层提交。
        if (write)
        {
            memmove(vbdev->ph_bbuf, cursor, chunk_bytes);
        }

        int rc = submit_sector_transfer(vbdev->dev,
                                        vbdev->ph_bbuf,
                                        sector_id,
                                        chunk_sectors,
                                        write);
        if (rc != EOK)
        {
            return rc;
        }

        if (!write)
        {
            memmove(cursor, vbdev->ph_bbuf, chunk_bytes);
        }

        cursor += chunk_bytes;
        sector_id += chunk_sectors;
        remaining_blocks -= chunk_blocks;
    }

    return EOK;
}

static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    return blockdev_rw_common(bdev, buf, blk_id, blk_cnt, false);
}

static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    return blockdev_rw_common(bdev, const_cast<void *>(buf), blk_id, blk_cnt, true);
}

int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    return blockdev_rw_common(bdev, buf, blk_id, blk_cnt, false);
}

int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    return blockdev_rw_common(bdev, const_cast<void *>(buf), blk_id, blk_cnt, true);
}

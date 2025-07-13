#include "vfs_ext4_blockdev_ext.hh"

#include "types.hh"

#include "libs/clist.h"
#include "spinlock.hh"
#include "fs/vfs/fs.hh"
#include "fs/buf.hh"

#if defined(QEMU)
#include "dev/virtio.h"
#endif

#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_blockdev.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/misc/queue.hh"
#include "libs/string.hh"

static int blockdev_lock(struct ext4_blockdev *bdev);

static int blockdev_unlock(struct ext4_blockdev *bdev);

static int blockdev_open(struct ext4_blockdev *bdev);

static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);

static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);

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

        bd = &vbdev->bd;
        bd->bdif = iface;
        bd->part_offset = 0;

        bd->part_size = (uint64)(512ull * 8ull * 1024ull * 1024ull );
        ph_bbuf = &vbdev->ph_bbuf[0];

        iface->lock = blockdev_lock;
        iface->unlock = blockdev_unlock;
        iface->open = blockdev_open;
        iface->bread = blockdev_read;
        iface->bwrite = blockdev_write;
        iface->close = blockdev_close;


        iface -> ph_bsize = BSIZE;

        iface->ph_bbuf = ph_bbuf;
        iface->ph_bcnt = bd->part_size / (uint64) bd->bdif->ph_bsize;

        printf("vfs_ext4_blockdev_init: ph_bsize=%p, ph_bcnt=%p\n", iface->ph_bsize, iface->ph_bcnt);
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

        bd = &vbdev->bd;
        bd->bdif = iface;
        bd->part_offset = 0;

        bd->part_size = 768 * 1024 * 1024;

        ph_bbuf = &vbdev->ph_bbuf[0];

        iface->lock = blockdev_lock;
        iface->unlock = blockdev_unlock;
        iface->open = blockdev_open;
        iface->bread = blockdev_read2;
        iface->bwrite = blockdev_write2;
        iface->close = blockdev_close;


        iface -> ph_bsize = BSIZE;

        iface->ph_bbuf = ph_bbuf;
        iface->ph_bcnt = bd->part_size / (uint64) bd->bdif->ph_bsize;

        printf("vfs_ext4_blockdev_init: ph_bsize=%p, ph_bcnt=%p\n", iface->ph_bsize, iface->ph_bcnt);
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
    //暂时什么都不干
    return EOK;
}

static int blockdev_lock(struct ext4_blockdev *bdev) {
    // acquire(&bdevice_lock);
    // while (__sync_lock_test_and_set(&bdevice_lock, 1) != 0) {
    // }

    return EOK;
}

static int blockdev_unlock(struct ext4_blockdev *bdev) {
    // __sync_lock_release(&bdevice_lock);
    return EOK;
}

static int blockdev_open(struct ext4_blockdev *bdev) { return EOK; }

static int blockdev_close(struct ext4_blockdev *bdev) { return EOK; }

static int blockdev_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    uint64 buf_ptr = (uint64)buf;
    for(int i = 0; i <(int) blk_cnt; i++) {
        // printf("[blockdev_bread] blk_id: %d, blk_cnt: %d\n", blk_id + i, blk_cnt);
        struct buf *b = bread(0, blk_id + i);
        memmove((void*)buf_ptr, b->data, BSIZE);
        buf_ptr += BSIZE;
        brelse(b);
    }
    return EOK;
}

static int blockdev_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt) {
	uint64 buf_ptr = (uint64)buf;
	for(int i = 0; i <(int) blk_cnt; i++) {
		// printf("[blockdev_bwrite] blk_id: %d, blk_cnt: %d\n", blk_id + i, blk_cnt);
		struct buf *b = bget(0, blk_id + i);
		memmove(b->data, (void*)buf_ptr, BSIZE);
		bwrite(b);
		buf_ptr += BSIZE;
		brelse(b);
	}
	return EOK;
}

//For rootfs
 int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    uint64 buf_ptr = (uint64)buf;
    for(int i = 0; i < (int)blk_cnt; i++) {
        // printf("[blockdev_bread] blk_id: %d, blk_cnt: %d\n", blk_id + i, blk_cnt);
        struct buf *b = bread(1, blk_id + i);
        memmove((void*)buf_ptr, b->data, BSIZE);
        buf_ptr += BSIZE;
        brelse(b);
    }
    return EOK;
}

 int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt) {
    uint64 buf_ptr = (uint64)buf;
    for(int i = 0; i < (int)blk_cnt; i++) {
        // printf("[blockdev_bwrite] blk_id: %d, blk_cnt: %d\n", blk_id + i, blk_cnt);
        struct buf *b = bget(1, blk_id + i);
        memmove(b->data, (void*)buf_ptr, BSIZE);
        bwrite(b);
        buf_ptr += BSIZE;
        brelse(b);
    }
    return EOK;
}













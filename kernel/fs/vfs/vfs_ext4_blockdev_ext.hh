#pragma once

#include "fs/lwext4/ext4_blockdev.hh"
#include "semaphore.hh"

struct vfs_ext4_blockdev {
  int dev;
  struct ext4_blockdev bd;
  sem io_sem;
  // ext4 的物理块缓冲区同时承担 DMA bounce buffer 的角色。
  // 这里改成“连续多页”缓冲区，既保留物理连续性，也允许把多个 4KiB 块
  // 合并成一次底层 virtio 传输，显著降低并发小块 IO 的请求数。
  uint8 *ph_bbuf;
  uint32 ph_bbuf_page_count;
};

#define DEV_NAME "virtio_disk"

struct vfs_ext4_blockdev *vfs_ext4_blockdev_create(int dev);
int vfs_ext4_blockdev_destroy(struct vfs_ext4_blockdev *bdev);
struct vfs_ext4_blockdev *vfs_ext4_blockdev_from_bd(struct ext4_blockdev *bd);

//For rootfs
[[maybe_unused]]  int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);
[[maybe_unused]]  int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);
struct vfs_ext4_blockdev *vfs_ext4_blockdev_create2(int dev);

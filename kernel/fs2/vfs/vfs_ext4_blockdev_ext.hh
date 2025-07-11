#pragma once

#include "lwext4/ext4_blockdev.hh"

struct vfs_ext4_blockdev {
  int dev;
  struct ext4_blockdev bd;
  uint8 ph_bbuf[4096];
};

#define DEV_NAME "virtio_disk"

struct vfs_ext4_blockdev *vfs_ext4_blockdev_create(int dev);
int vfs_ext4_blockdev_destroy(struct vfs_ext4_blockdev *bdev);
struct vfs_ext4_blockdev *vfs_ext4_blockdev_from_bd(struct ext4_blockdev *bd);

//For rootfs
static int blockdev_write2(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt);
static int blockdev_read2(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt);
struct vfs_ext4_blockdev *vfs_ext4_blockdev_create2(int dev);
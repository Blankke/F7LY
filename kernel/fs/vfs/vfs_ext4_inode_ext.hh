#pragma once

#include "fs/lwext4/ext4_fs.hh"
#include "fs/lwext4/ext4_types.hh"

#define EXT4_PATH_LONG_MAX 512

struct vfs_ext4_inode_info {
    char fname[EXT4_PATH_LONG_MAX];
};


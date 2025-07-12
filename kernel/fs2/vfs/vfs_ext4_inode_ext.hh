#pragma once

#include "fs2/lwext4/ext4_fs.h"
#include "fs2/lwext4/ext4_types.h"

#define EXT4_PATH_LONG_MAX 512

struct vfs_ext4_inode_info {
    char fname[EXT4_PATH_LONG_MAX];
};


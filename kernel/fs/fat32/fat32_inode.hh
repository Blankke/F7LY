#pragma once

struct fat32_entry;

struct vfs_fat32_inode_info {
    struct fat32_entry *entry;
};

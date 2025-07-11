#pragma once

#include "fs/vfs/inode.hh"
#include "param.h"
#include "spinlock.hh"

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 50
#define ROOTINO  1   // root i-number
#define BSIZE 512
#define CONSOLE 1

typedef enum {
    FAT32 = 1,
    EXT4 = 2,
} fs_t;

struct filesystem;

struct filesystem_op {
    int (*mount)(struct filesystem *fs, unsigned long rwflag, void *data);
    int (*umount)(struct filesystem *fs);
    int (*statfs)(struct filesystem *fs, struct statfs *buf);
};

typedef struct filesystem_op filesystem_op_t;

struct filesystem {
    int dev; //设备号
    fs_t type;
    struct filesystem_op *fs_op;
    char *path;
    void *fs_data;
};

typedef struct filesystem filesystem_t;

extern filesystem_t *fs_table[VFS_MAX_FS];
extern filesystem_op_t *fs_ops_table[VFS_MAX_FS];
extern struct spinlock fs_table_lock;

void filesystem_init(void);
void filesystem2_init(void);
void init_fs_table(void);
void fs_init(filesystem_t *fs, int dev, fs_t fs_type, const char *path);

filesystem_t *get_fs_from_path(const char *path);
/*
 *TODO:
 *支持挂载
 */
int fs_mount(filesystem_t *fs, uint64 rwflag, void *data);
int fs_umount(filesystem_t *fs);
filesystem_t *get_fs_by_type(fs_t type);
filesystem_t *get_fs_by_mount_point(const char *path);










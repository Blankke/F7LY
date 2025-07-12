#include "fs2/vfs/fs.hh"



#include "types.hh"
#include "platform.hh"
#include "param.h"
#include "vfs_ext4_ext.hh"
#include "libs/string.hh"
#include "proc_manager.hh"
#include "fs2/vfs/ops.hh"


filesystem_t *fs_table[VFS_MAX_FS];
filesystem_op_t *fs_ops_table[VFS_MAX_FS] = {
    NULL,
    NULL, //remain for FAT32 if needed
    &ext4_fs_op,
    NULL,
};

filesystem_t ext4_fs;

filesystem_t root_fs; // 仅用来加载init程序

SpinLock fs_table_lock;

void init_fs_table(void) {
    fs_table_lock.init( "fs_table_lock");
    for (int i = 0; i < VFS_MAX_FS; i++) {
        fs_table[i] = NULL;
    }
    fs_table[EXT4] = &ext4_fs;
    printf("init_fs_table finished\n");
}

void fs_init(filesystem_t *fs, int dev, fs_t fs_type, const char *path) {
    fs_table_lock.acquire();
    fs_table[fs_type] = fs;
    fs->dev = dev;
    fs->type = fs_type;
    printf("fs_init:%p ,path:%d, path: %s\n", fs->path, path);
    char tmp[strlen(path) + 1];
    fs->path=tmp; /* path should be a string literal */
    strcpy(fs->path, path);
    fs->fs_op = fs_ops_table[fs_type];
   fs_table_lock.release();
    printf("fs_init done\n");
}

void filesystem_init(void) {
    fs_init(&ext4_fs, ROOTDEV, EXT4, "/mnt/"); // 应为"/"
    void *fs_sb = NULL;
    fs_mount(&ext4_fs, 0, fs_sb);

    // int r = ext4_cache_write_back("/", true);
    // if (r != EOK) {
    //     panic("ext4_cache write back error!\n");
    // }

    struct file_vnode *cwd = &(proc::k_pm.get_cur_pcb()->cwd);
    strcpy(cwd->path, "/");
    cwd->fs = &ext4_fs;
}

void filesystem2_init(void) {
    fs_table_lock.acquire();
    fs_table[3] = &root_fs;
    root_fs.dev = 2;
    root_fs.type = EXT4;
    root_fs.fs_op = fs_ops_table[root_fs.type];
    strcpy(root_fs.path, "/");
   fs_table_lock.release();
    int ret = vfs_ext_mount2(&root_fs, 0, NULL);
    printf("fs_mount done: %d\n", ret);

}

int fs_mount(filesystem_t *fs, uint64 rwflag, void *data) {
    if (fs -> fs_op -> mount) {
        int ret = fs -> fs_op -> mount(fs, rwflag, data);
        printf("fs_mount done: %d\n", ret);
        return ret;
    }
    return -1;
}

/**
 * TODO: not implemented yet
 */
int fs_umount(filesystem_t *fs) { return 0; }

filesystem_t *get_fs_by_type(fs_t type) {
    if (fs_table[type]) {
        return fs_table[type];
    }
    return NULL;
}

filesystem_t *get_fs_by_mount_point(const char *mp) {
    for (int i = 0; i < VFS_MAX_FS; i++) {
        if (fs_table[i] && fs_table[i]->path && !strcmp(fs_table[i]->path, mp)) {
            return fs_table[i];
        }
    }
    return NULL;
}

struct filesystem *get_fs_from_path(const char *path) {
    char abs_path[MAXPATH] = {0};
    get_absolute_path(path, "/", abs_path);

    size_t len = strlen(abs_path);
    char *pstart = abs_path, *pend = abs_path + len - 1;
    while (pend > pstart) {
        if (*pend == '/') {
            *pend = '\0';
            filesystem_t *fs = get_fs_by_mount_point(pstart);
            if (fs) {
                return fs;
            }
        }
        pend--;
    }

    if (pend == pstart) {
        return get_fs_by_mount_point("/");
    }

    return NULL;
}












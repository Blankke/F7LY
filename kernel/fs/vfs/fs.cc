#include "fs/vfs/fs.hh"

#include "types.hh"
#include "platform.hh"
#include "param.h"
#include "vfs_ext4_ext.hh"
#include "libs/string.hh"
#include "proc_manager.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "proc/meminfo.hh"
#include "proc/cpuinfo.hh"
#include "fs/fat32/fat32.hh"

#include "devs/ramdisk.hh"
#include "devs/dtb.hh"
#include "devs/device_manager.hh"

filesystem_t *fs_table[VFS_MAX_FS];
filesystem_op_t *fs_ops_table[VFS_MAX_FS] = {
    NULL,
    &fat32_fs_op, 
    &ext4_fs_op,
    NULL,
};

filesystem_t ext4_fs;
filesystem_t fat32_fs;

filesystem_t root_fs; // 仅用来加载init程序

SpinLock fs_table_lock;

void init_fs_table(void)
{
    fs_table_lock.init("fs_table_lock");
    for (int i = 0; i < VFS_MAX_FS; i++)
    {
        fs_table[i] = NULL;
    }
    fs_table[EXT4] = &ext4_fs;
    fs_table[FAT32] = &fat32_fs;
    printf("init_fs_table finished\n");
}

void fs_init(filesystem_t *fs, int dev, fs_t fs_type, char *path)
{
    fs_table_lock.acquire();
    fs_table[fs_type] = fs;
    fs->dev = dev;
    fs->type = fs_type;
    fs->path = path; /* path should be a string literal */
    printf("fs->path: %s\n", fs->path);
    fs->fs_op = fs_ops_table[fs_type];
    fs_table_lock.release();
    printf("fs_init done\n");
}

extern uint64 k_dtb_addr;

void filesystem_init(void)
{
    // Try to init DTB
    if (k_dtb_addr) {
        DtbManager::init(k_dtb_addr);
    }
    printfYellow("filesystem_init: DTB initialized at 0x%lx\n", k_dtb_addr);
    uint64 start = 0, end = 0;
    int root_dev_id = ROOTDEV; // Default
    
    if (DtbManager::get_initrd(start, end) && start != 0) {
        printfYellow("Found Initrd: 0x%lx - 0x%lx, size: %ld\n", start, end, end - start);
        // Using new to avoid static destructor registration (__dso_handle issue)
        dev::RamDisk* ramdisk = new dev::RamDisk(start, end - start);
        if (ramdisk) {
            root_dev_id = dev::k_devm.register_block_device(ramdisk, "ramdisk");
            printfYellow("Registered RamDisk as device %d\n", root_dev_id);
        } else {
             printfRed("Failed to allocate RamDisk!\n");
        }
    } else {
        printfRed("Initrd not found, using default ROOTDEV\n");
    }

    static char root_path[] = "/";
    printfYellow("===========Mounted EXT4 on %s (dev %d)\n", root_path, root_dev_id);
    fs_mount(root_dev_id, EXT4, root_path, 0, NULL); // 挂载文件系统
    dir_init();
    
    static char fat32_path[] = "/fat32";
    int a =vfs_mkdir(fat32_path, 0777);
    int r =vfs_is_file_exist(fat32_path) ;
    printfYellow("vfs_mkdir /fat32 result: %d, exist check: %d\n", a, r);
    if (a==0 || r== 1) {
        // Mount FAT32 on device 0
        fs_mount(0, FAT32, fat32_path, 0, NULL);
        printf("Mounted FAT32 on %s\n", fat32_path);
    }
}

void dir_init(void)
{
    struct inode *ip;

    if ((ip = namei((char *)"/dev/misc/rtc")) == NULL)
        vfs_ext_mkdir((char *)"/dev/misc/rtc", 0777);
    else
        free_inode(ip);


    if ((ip = namei((char *)"/tmp")) != NULL)
    {
        vfs_ext_rm((char *)"tmp");
        free_inode(ip);
    }

    if ((ip = namei((char *)"/usr")) == NULL)
        vfs_ext_mkdir((char *)"/usr", 0777);
    else
        free_inode(ip);

    if ((ip = namei((char *)"/usr/lib")) == NULL)
        vfs_ext_mkdir((char *)"/usr/lib", 0777);
    else
        free_inode(ip);
}

void filesystem2_init(void)
{
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

int fs_mount(int dev, fs_t fs_type,
             char *path, uint64 rwflag, void *data)
{
    fs_register(dev, fs_type, path);
    filesystem_t *fs = get_fs_by_type(fs_type);
    if (fs->fs_op->mount)
    {
        int ret = fs->fs_op->mount(fs, rwflag, data);
        return ret;
    }
    return -1;
}

void fs_register(int dev, fs_t fs_type, char *path)
{
    fs_table_lock.acquire();
    filesystem_t *fs = get_fs_by_type(fs_type);
    fs->dev = dev;
    fs->type = fs_type;
    fs->path = path; /* path should be a string literal */
    fs->fs_op = fs_ops_table[fs_type];
    fs_table_lock.release();
}

/**
 * TODO: not implemented yet
 */
int fs_umount(filesystem_t *fs) { return 0; }

filesystem_t *get_fs_by_type(fs_t type)
{
    if (fs_table[type])
    {
        return fs_table[type];
    }
    return NULL;
}

filesystem_t *get_fs_by_mount_point(const char *mp)
{
    for (int i = 0; i < VFS_MAX_FS; i++)
    {
        if (fs_table[i] && fs_table[i]->path && !strcmp(fs_table[i]->path, mp))
        {
            return fs_table[i];
        }
    }
    return NULL;
}

struct filesystem *get_fs_from_path(const char *path)
{
    char abs_path[MAXPATH] = {0};
    get_absolute_path(path, "/", abs_path);

    size_t len = strlen(abs_path);
    char *pstart = abs_path, *pend = abs_path + len - 1;
    while (pend > pstart)
    {
        if (*pend == '/')
        {
            *pend = '\0';
            filesystem_t *fs = get_fs_by_mount_point(pstart);
            if (fs)
            {
                return fs;
            }
        }
        pend--;
    }

    if (pend == pstart)
    {
        return get_fs_by_mount_point("/");
    }

    return NULL;
}

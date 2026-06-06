#include "fs/vfs/fs.hh"

#include "types.hh"
#include "platform.hh"
#include "param.h"
#include "vfs_ext4_ext.hh"
#include "libs/string.hh"
#include "proc_manager.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "proc/meminfo.hh"
#include "proc/cpuinfo.hh"
#include "fs/fat32/fat32.hh"

#include "devs/ramdisk.hh"
#include "devs/dtb.hh"
#include "devs/device_manager.hh"
#include "fs/buf.hh"

namespace
{

constexpr size_t k_boot_path_capacity = 4096;

/**
 * 清空启动期临时目录，但保留目录本身。
 *
 * /dev/shm 在 Linux 中由 tmpfs 提供，重启后必须为空。当前内核暂时使用
 * 根 ext4 承载共享映射，因此需要在挂载根文件系统后恢复同样的生命周期语义。
 * 每轮只取一个目录项并关闭迭代器，再执行删除，避免遍历期间修改目录导致游标失效。
 */
int clear_ephemeral_directory(const char *directory)
{
    while (true)
    {
        ext4_dir dir{};
        int open_ret = ext4_dir_open(&dir, directory);
        if (open_ret != EOK)
            return -open_ret;

        char entry_name[EXT4_DIRECTORY_FILENAME_LEN + 1] = {};
        bool found = false;
        const ext4_direntry *entry = nullptr;
        while ((entry = ext4_dir_entry_next(&dir)) != nullptr)
        {
            size_t name_len = entry->name_length;
            if (name_len == 0 || name_len > EXT4_DIRECTORY_FILENAME_LEN)
            {
                (void)ext4_dir_close(&dir);
                return -EIO;
            }
            if ((name_len == 1 && entry->name[0] == '.') ||
                (name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.'))
            {
                continue;
            }

            memmove(entry_name, entry->name, name_len);
            entry_name[name_len] = '\0';
            found = true;
            break;
        }
        (void)ext4_dir_close(&dir);

        if (!found)
            return EOK;

        size_t directory_len = strlen(directory);
        size_t entry_len = strlen(entry_name);
        bool needs_separator = directory_len == 0 || directory[directory_len - 1] != '/';
        if (directory_len + (needs_separator ? 1 : 0) + entry_len + 1 >
            k_boot_path_capacity)
        {
            return -ENAMETOOLONG;
        }

        char child_path[k_boot_path_capacity] = {};
        memmove(child_path, directory, directory_len);
        size_t offset = directory_len;
        if (needs_separator)
            child_path[offset++] = '/';
        memmove(child_path + offset, entry_name, entry_len);
        child_path[offset + entry_len] = '\0';

        int unlink_ret = vfs_ext_unlink(child_path);
        if (unlink_ret == -EISDIR)
        {
            int clear_ret = clear_ephemeral_directory(child_path);
            if (clear_ret < 0)
                return clear_ret;
            int rmdir_ret = vfs_ext_rmdir(child_path);
            if (rmdir_ret < 0)
                return rmdir_ret;
        }
        else if (unlink_ret < 0)
        {
            return unlink_ret;
        }
    }
}

enum class block_fs_kind
{
    unknown = 0,
    fat32,
    ext4,
};

const char *block_fs_kind_name(block_fs_kind kind)
{
    switch (kind)
    {
    case block_fs_kind::fat32:
        return "FAT32";
    case block_fs_kind::ext4:
        return "EXT4";
    default:
        return "unknown";
    }
}

/**
 * 启动阶段先轻量探测块设备格式，再决定走哪条挂载路径。
 * 这样 FAT32 识别失败时就不会再把整个启动流程直接 panic 掉。
 */
block_fs_kind probe_block_fs_kind(int dev)
{
    if (dev < 0)
    {
        return block_fs_kind::unknown;
    }

    if (fat32_probe_device(dev) == 0)
    {
        return block_fs_kind::fat32;
    }

    struct buf *super = bread(dev, 2);
    if (super == NULL)
    {
        return block_fs_kind::unknown;
    }

    uint16 ext4_magic = 0;
    memmove(&ext4_magic, super->data + 56, sizeof(ext4_magic));
    brelse(super);

    if (ext4_magic == 0xEF53)
    {
        return block_fs_kind::ext4;
    }

    return block_fs_kind::unknown;
}

} // namespace

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
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;

void filesystem_init(void)
{
    // Try to init DTB
    if (k_dtb_addr) {
        DtbManager::init(k_dtb_addr);
    }
    printfBlue("[fs] DTB 已初始化，地址=0x%lx\n", k_dtb_addr);
    printfBlue("[fs] initrd 扫描结果：start=0x%lx end=0x%lx\n", k_initrd_start, k_initrd_end);
    
    uint64 start = 0, end = 0;
    int ramdisk_dev_id = -1;
    
    if (DtbManager::get_initrd(start, end) && start != 0) {
        printfBlue("[fs] 从 DTB 找到 initrd：0x%lx - 0x%lx，大小=%ld\n", start, end, end - start);
        // Using new to avoid static destructor registration (__dso_handle issue)
        uint64 ramdisk_start = start;
        #ifdef LOONGARCH
        ramdisk_start = to_vir(start);
        #endif
        dev::RamDisk* ramdisk = new dev::RamDisk(ramdisk_start, end - start);
        if (ramdisk) {
            ramdisk_dev_id = dev::k_devm.register_block_device(ramdisk, "ramdisk");
            printfGreen("[fs] initrd 已注册为块设备 %d\n", ramdisk_dev_id);
        } else {
             printfRed("[fs] RamDisk 分配失败\n");
        }
    } else if (k_initrd_start != 0 && k_initrd_end > k_initrd_start) {
        printfYellow("[fs] DTB 未给出 initrd，回退到扫描结果：0x%lx - 0x%lx\n", k_initrd_start, k_initrd_end);
        start = k_initrd_start;
        end = k_initrd_end;
        
        uint64 ramdisk_start = start;
        #ifdef LOONGARCH
        ramdisk_start = to_vir(start);
        #endif
        dev::RamDisk* ramdisk = new dev::RamDisk(ramdisk_start, end - start);
        if (ramdisk) {
            ramdisk_dev_id = dev::k_devm.register_block_device(ramdisk, "ramdisk");
            printfGreen("[fs] 扫描到的 initrd 已注册为块设备 %d\n", ramdisk_dev_id);
        }
    } else {
        printfYellow("[fs] 未找到 initrd，将直接探测主块设备\n");
    }

    block_fs_kind primary_dev_kind = probe_block_fs_kind(ROOTDEV);
    block_fs_kind ramdisk_kind = probe_block_fs_kind(ramdisk_dev_id);
    printfBlue("[fs] 设备 %d 识别为 %s\n", ROOTDEV, block_fs_kind_name(primary_dev_kind));
    if (ramdisk_dev_id >= 0)
    {
        printfBlue("[fs] 设备 %d 识别为 %s\n", ramdisk_dev_id, block_fs_kind_name(ramdisk_kind));
    }

    int root_dev_id = -1;
    fs_t root_fs_type = EXT4;
    int fat32_data_dev = -1;

    if (primary_dev_kind == block_fs_kind::ext4)
    {
        root_dev_id = ROOTDEV;
        printfGreen("[fs] 使用设备 %d 上的 EXT4 作为根文件系统\n", root_dev_id);
        if (ramdisk_dev_id >= 0 && ramdisk_kind == block_fs_kind::ext4)
        {
            printfYellow("[fs] 检测到 ext4 initrd，但主盘已可直接启动，initrd 本轮不挂载\n");
        }
    }
    else if (primary_dev_kind == block_fs_kind::fat32)
    {
        fat32_data_dev = ROOTDEV;
        if (ramdisk_kind == block_fs_kind::ext4)
        {
            root_dev_id = ramdisk_dev_id;
            printfYellow("[fs] 主盘是 FAT32，回退到设备 %d 上的 EXT4 initrd 作为根文件系统\n", root_dev_id);
        }
    }
    else if (ramdisk_kind == block_fs_kind::ext4)
    {
        root_dev_id = ramdisk_dev_id;
        printfYellow("[fs] 主盘格式未知，回退到设备 %d 上的 EXT4 initrd\n", root_dev_id);
    }

    if (root_dev_id < 0)
    {
        printfRed("[fs] 没有找到可启动的 EXT4 根文件系统：dev0=%s initrd=%s\n",
                  block_fs_kind_name(primary_dev_kind), block_fs_kind_name(ramdisk_kind));
        panic("filesystem_init: no bootable EXT4 root filesystem");
    }

    static char root_path[] = "/";
    int root_mount_ret = fs_mount(root_dev_id, root_fs_type, root_path, 0, NULL);
    if (root_mount_ret != 0)
    {
        printfRed("[fs] 根文件系统挂载失败：dev=%d type=%s ret=%d\n",
                  root_dev_id, block_fs_kind_name(block_fs_kind::ext4), root_mount_ret);
        panic("filesystem_init: root mount failed");
    }
    printfGreen("[fs] 根文件系统已挂载到 %s (dev=%d, type=%s)\n",
                root_path, root_dev_id, block_fs_kind_name(block_fs_kind::ext4));
    dir_init();
    
    if (fat32_data_dev >= 0)
    {
        static char fat32_path[] = "/fat32";
        int mkdir_ret = vfs_mkdir(fat32_path, 0777);
        int exists_ret = vfs_is_file_exist(fat32_path);
        if (mkdir_ret == 0 || exists_ret == 1)
        {
            int fat32_mount_ret = fs_mount(fat32_data_dev, FAT32, fat32_path, 0, NULL);
            if (fat32_mount_ret == 0)
            {
                printfGreen("[fs] FAT32 数据盘已挂载到 %s (dev=%d)\n", fat32_path, fat32_data_dev);
            }
            else
            {
                printfRed("[fs] FAT32 数据盘挂载失败：dev=%d ret=%d\n", fat32_data_dev, fat32_mount_ret);
            }
        }
        else
        {
            printfRed("[fs] 无法创建 FAT32 挂载点 %s：mkdir=%d exist=%d\n",
                      fat32_path, mkdir_ret, exists_ret);
        }
    }
}

void dir_init(void)
{
    struct inode *ip;

    // /dev/misc 需要作为目录存在，/dev/misc/rtc 本身应该是设备节点，
    // 不能再被误建成目录，否则用户态会把 RTC 当成目录打开。
    if ((ip = namei((char *)"/dev/misc")) == NULL)
        vfs_ext_mkdir((char *)"/dev/misc", 0777);
    else
        free_inode(ip);

    // POSIX shm_open/sem_open 约定走 /dev/shm。
    // 没有这个目录时，musl 会在打开共享内存对象前就直接 ENOENT，
    // pthread_cancel_points 里的 shm_open 场景也会因此被异常路径干扰。
    if ((ip = namei((char *)"/dev/shm")) == NULL)
        vfs_ext_mkdir((char *)"/dev/shm", 0777);
    else
        free_inode(ip);

    int shm_clear_ret = clear_ephemeral_directory("/dev/shm");
    if (shm_clear_ret < 0)
    {
        printfRed("[fs] 清空 /dev/shm 失败: %d\n", shm_clear_ret);
    }


    // libc 的 tmpfile/mkstemp 等接口默认依赖 /tmp。
    // 官方评测镜像必须原样使用，启动时只能补齐运行环境，不能删除镜像内已有目录。
    if ((ip = namei((char *)"/tmp")) == NULL)
        vfs_ext_mkdir((char *)"/tmp", 01777);
    else
        free_inode(ip);

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
    eastl::string abs_path = get_absolute_path(path, "/");

    // 优先检查完全匹配挂载点的情况
    filesystem_t *exact_fs = get_fs_by_mount_point(abs_path.c_str());
    if (exact_fs)
    {
        return exact_fs;
    }

    while (abs_path != "/")
    {
        size_t last_slash = abs_path.find_last_of('/');
        if (last_slash == eastl::string::npos || last_slash == 0)
        {
            break;
        }

        abs_path = abs_path.substr(0, last_slash);
        filesystem_t *fs = get_fs_by_mount_point(abs_path.c_str());
        if (fs)
        {
            return fs;
        }
    }

    return get_fs_by_mount_point("/");
}

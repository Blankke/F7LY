#pragma once

#include "time.h"
#include "fs/vfs/file.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/inode.hh"
#include "fs/lwext4/ext4.hh"

/**
 * 对直接访问 lwext4 低层 inode/cache 接口的 VFS 路径提供统一保护。
 *
 * lwext4 的公开路径会自行调用 mount 的 os_locks，但 ext4_fs_* 低层接口不会。
 * 该 guard 使用同一把可重入挂载锁，因此允许受保护代码再进入公开 lwext4 接口。
 */
class Ext4MountGuard
{
public:
    explicit Ext4MountGuard(struct ext4_mountpoint *mountpoint)
        : _mountpoint(mountpoint)
    {
        if (_mountpoint != nullptr && _mountpoint->os_locks != nullptr)
        {
            _mountpoint->os_locks->lock();
        }
    }

    ~Ext4MountGuard()
    {
        if (_mountpoint != nullptr && _mountpoint->os_locks != nullptr)
        {
            _mountpoint->os_locks->unlock();
        }
    }

    Ext4MountGuard(const Ext4MountGuard &) = delete;
    Ext4MountGuard &operator=(const Ext4MountGuard &) = delete;

private:
    struct ext4_mountpoint *_mountpoint;
};

struct linux_dirent64 {
    uint64 d_ino;
    int64 d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[0];
};

// linux_dirent64.d_type 对外必须使用 Linux DT_* ABI 值，不能复用内核内部 T_*。
// 例如内核 T_FILE 当前为 2，而 Linux DT_REG 为 8；直接透传会让 Git 误判文件类型。
#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_FIFO    1
#define LINUX_DT_CHR     2
#define LINUX_DT_DIR     4
#define LINUX_DT_BLK     6
#define LINUX_DT_REG     8
#define LINUX_DT_LNK     10
#define LINUX_DT_SOCK    12

int vfs_ext4_init(void);


//fs operations
int vfs_ext_mount(struct filesystem *fs, uint64_t rwflag, void *data);
int vfs_ext_mount2(struct filesystem *fs, uint64_t rwflag, void *data);
int vfs_ext_umount(struct filesystem *fs);
int vfs_ext_fstat(struct file *f, struct kstat *st);
int vfs_ext_flush(struct filesystem *fs);

extern struct filesystem_op ext4_fs_op;

//file operations
int vfs_ext_openat(struct file *f);
int vfs_ext_fclose(struct file *f);
int vfs_ext_read(struct file *f, int is_user_addr, const uint64 addr, int n);
int vfs_ext_readat(struct file *f, int is_user_addr, const uint64 addr, int n, int offset);
int vfs_ext_write(struct file *f, int is_user_addr, const uint64 addr, int n);
int vfs_ext_fflush(struct file *f);
int vfs_ext_link(const char *oldpath, const char *newpath);
int vfs_ext_rm(const char *path);
int vfs_ext_stat(const char *path, struct kstat *st);
int vfs_ext_fstat(struct file *f, struct kstat *st);
int vfs_ext_statx(struct file *f, struct statx *st);
int vfs_ext_mkdir(const char *path, uint64_t mode);
int vfs_ext_is_dir(const char *path);
int vfs_ext_dirclose(struct file *f);
int vfs_ext_getdents(struct file *f, struct linux_dirent64 *dirp, int count);
int vfs_ext_readlink(const char *path, uint64 ubuf, size_t bufsize);
int vfs_ext_faccessat(struct file *f, int mode);
int vfs_ext_lseek(struct file *f, int offset, int whence);
int vfs_ext_frename(const char *oldpath, const char *newpath);
int vfs_ext_get_filesize(const char *path, uint64_t *size);
int vfs_ext_futimens(struct file *f, const struct timespecc *ts);
int vfs_ext_utimens(const char *path, const struct timespecc *ts);

//inode operations
struct inode *vfs_ext_namei(const char *name);
ssize_t vfs_ext_readi(struct inode *self, int user_dst, uint64 addr, uint off, uint n);
void vfs_ext_locki(struct inode *self);
void vfs_ext_unlock_puti(struct inode *self);
extern struct inode_operations ext4_inode_op;
struct inode_operations *get_ext4_inode_op(void);
int vfs_ext_mknod(const char *path, uint32 mode, uint32 dev);
int vfs_ext_symlink(const char *target, const char *path);
int vfs_ext_unlink(const char *path);
int vfs_ext_rmdir(const char *path);



/*
 *时间单位转换
 */
#define NS_to_S(ns) (ns / (1000000000))
#define S_to_NS(s) (s * 1UL * 1000000000)

























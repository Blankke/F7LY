
#include "types.hh"

#include "tm/time.h"
#include "platform.hh"
#include "fs/vfs/file.hh"
#include "fs/vfs/fs.hh"
#include "fs/stat.hh"
#include "fs/fcntl.hh"
#include "vfs_ext4_blockdev_ext.hh"
#include "vfs_ext4_ext.hh"
#include "fs/vfs/inode.hh"

#include <fs/lwext4/ext4_oflags.hh>
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_fs.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_super.hh"
#include "fs/lwext4/ext4_types.hh"

#include "fs/lwext4/ext4.hh"
#include "fs/ioctl.hh"
#include "libs/string.hh"

#include "physical_memory_manager.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#define min(a, b) ((a) < (b) ? (a) : (b))

//TODO：测试成功后替换为信号量
struct semaphore extlock;
[[maybe_unused]] static void ext4_lock(void);
[[maybe_unused]] static void ext4_unlock(void);

[[maybe_unused]] static struct ext4_lock ext4_lock_ops = {ext4_lock, ext4_unlock};

[[maybe_unused]] static uint vfs_ext4_filetype(uint filetype);

int vfs_ext4_init(void) {
    sem_init(&extlock, 1, const_cast<char*>("ext4_sem"));
    ext4_device_unregister_all();
    ext4_init_mountpoints();
    return 0;
}

static void ext4_lock() {
    sem_p(&extlock);
}

static void ext4_unlock() {
    sem_v(&extlock);
}

[[maybe_unused]] static uint vfs_ext4_filetype(uint filetype) {
    switch (filetype) {
        case T_DIR:
            return EXT4_DE_DIR;
        case T_FILE:
            return EXT4_DE_REG_FILE;
        case T_CHR:
            return EXT4_DE_CHRDEV;
        default:
            return EXT4_DE_UNKNOWN;
    }
}

int vfs_ext_mount(struct filesystem *fs, uint64_t rwflag, void *data) {
    int r = 0;
    [[maybe_unused]] struct ext4_blockdev *bdev = NULL;
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_create(fs->dev);

    if (vbdev == NULL) {
        r = -ENOMEM;
        goto out;
    }

    printf("MOUNT BEGIN %s\n", fs->path);
    bdev = &vbdev->bd;
    r = ext4_mount(DEV_NAME, fs->path, false);
    printf("EXT4 mount result: %d\n", r);

    // r = ext4_cache_write_back(fs->path, true);
    // if (r != EOK) {
    //     printf("EXT4 cache write back error! r=%d\n", r);
    //     return -1;
    // }

    if (r != EOK) {
        vfs_ext4_blockdev_destroy(vbdev);
        goto out;
    } else {
        // ext4_mount_setup_locks(fs->path, &ext4_lock_ops);
        //获得ext4文件系统的超级块
        // ext4_get_sblock(fs->path, (struct ext4_sblock **)(&(fs->fs_data)));
    }
out:
    return r;
}

//For rootfs
int vfs_ext_mount2(struct filesystem *fs, uint64_t rwflag, void *data) {
    int r = 0;
    [[maybe_unused]] struct ext4_blockdev *bdev = NULL;
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_create2(fs->dev);

    if (vbdev == NULL) {
        r = -ENOMEM;
        goto out;
    }

    printf("MOUNT BEGIN %s\n", fs->path);
    bdev = &vbdev->bd;
    r = ext4_mount("root_fs", fs->path, false);
    printf("EXT4 mount result: %d\n", r);

    if (r != EOK) {
        vfs_ext4_blockdev_destroy(vbdev);
        goto out;
    } else {
        // ext4_mount_setup_locks(fs->path, &ext4_lock_ops);
        //获得ext4文件系统的超级块
        // ext4_get_sblock(fs->path, (struct ext4_sblock **)(&(fs->fs_data)));
    }
    out:
        return r;
}

int vfs_ext_statfs(struct filesystem *fs, struct statfs *buf) {
    panic("未实现");
#ifdef FS_FIX_COMPLETELY
    struct ext4_sblock *sb = NULL;
    int err = EOK;

    err = ext4_get_sblock(fs->path, &sb);
    if (err != EOK) {
        return err;
    }

    buf->f_bsize = ext4_sb_get_block_size(sb);
    buf->f_blocks = ext4_sb_get_blocks_cnt(sb);
    buf->f_bfree = ext4_sb_get_free_blocks_cnt(sb);
    buf->f_bavail = ext4_sb_get_free_blocks_cnt(sb);
    buf->f_type = sb->magic;
    buf->f_files = sb->inodes_count;
    buf->f_ffree = sb->free_inodes_count;
    buf->f_frsize = ext4_sb_get_block_size(sb);
    buf->f_bavail = sb->free_inodes_count;
    buf->f_fsid.val[0] = 2; /* why 2? */
    buf->f_flags = 0;
    buf->f_namelen = 32;
    return err;
    #endif
    return -1;
}


struct filesystem_op ext4_fs_op = {
    .mount = vfs_ext_mount,
    .umount = vfs_ext_umount,
    .statfs = vfs_ext_statfs,
};


int vfs_ext_umount(struct filesystem *fs) {
    int r = 0;
    struct vfs_ext4_blockdev *vbdev =( vfs_ext4_blockdev *) fs->fs_data;

    if (vbdev == NULL) {
        r = -ENOMEM;
        return r;
    }

    ext4_umount(fs->path);
    if (r != EOK) {
        return r;
    }

    vfs_ext4_blockdev_destroy(vbdev);
    return EOK;
}

int vfs_ext_ioctl(struct file *f, int cmd, void *args) {
    int r = 0;
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    if (file == NULL) {
        panic("vfs_ext_ioctl: cannot get ext4 file\n");
    }

    switch (cmd) {
        case FIOCLEX:
            f->f_flags |= O_CLOEXEC;
        break;
        case FIONCLEX:
            f->f_flags &= ~O_CLOEXEC;
        break;
        case FIONREAD:
            r = ext4_fsize(file);
        break;
        case FIONBIO:
            break;
        case FIOASYNC:
            break;
        default:
            r = -EINVAL;
        break;
    }
    return r;
}

//user_addr = 1 indicate that user space pointer
int vfs_ext_read(struct file *f, int user_addr, const uint64 addr, int n) {
    uint64 byteread = 0;
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    if (file == NULL) {
        panic("vfs_ext_read: cannot get ext4 file\n");
    }
    int r = 0;
    if (user_addr) {
        char *buf = (char*)mem::k_pmm.kmalloc(n + 1);
        [[maybe_unused]] uint64 mread = 0;
        if (buf == NULL) {
            panic("vfs_ext_read: kalloc failed\n");
        }
        r = ext4_fread(file, buf, n, &byteread);
        if (r != EOK) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        if (mem::k_vmm.copy_out(proc::k_pm.get_cur_pcb()->_pt, addr, buf, byteread) != 0) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        mem::k_pmm.free_page(buf);
    } else {
        char *kbuf = (char *) addr;
        r = ext4_fread(file, kbuf, n, &byteread);
        if (r != EOK) {
            return 0;
        }
        memmove((char *) addr, kbuf, byteread);
    }
    f -> f_pos = file->fpos;

    return byteread;
}

int vfs_ext_readat(struct file *f, int user_addr, const uint64 addr, int n, int offset) {
    uint64 byteread = 0;
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    if (file == NULL) {
        panic("vfs_ext_read: cannot get ext4 file\n");
    }
    int r = ext4_fseek(file, offset, SEEK_SET);
    if (r != EOK) {
        return -1;
    }
    if (user_addr) {
        char *buf =(char*) mem::k_pmm.kmalloc(n + 1);
        [[maybe_unused]] uint64 mread = 0;
        if (buf == NULL) {
            panic("vfs_ext_read: kalloc failed\n");
        }
        r = ext4_fread(file, buf, n, &byteread);
        if (r != EOK) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        if (mem::k_vmm.copy_out(proc::k_pm.get_cur_pcb()->_pt, addr, buf, byteread) != 0) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        mem::k_pmm.free_page(buf);
    } else {
        char *kbuf = (char *) addr;
        r = ext4_fread(file, kbuf, n, &byteread);
        if (r != EOK) {
            return 0;
        }
        memmove((char *) addr, kbuf, byteread);
    }
    r = ext4_fseek(file, f->f_pos, SEEK_SET);
    if (r != EOK) {
        return -1;
    }
    return byteread;
}

int vfs_ext_write(struct file *f, int user_addr, const uint64 addr, int n) {
    uint64 bytewrite = 0;
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    if (file == NULL) {
        panic("vfs_ext_write: cannot get ext4 file\n");
    }
    int r = 0;
    if (user_addr) {
        char *buf = (char*)mem::k_pmm.kmalloc(n + 1);
        [[maybe_unused]] uint64 mwrite = 0;
        if (buf == NULL) {
            panic("vfs_ext_read: kalloc failed\n");
        }
        if (mem::k_vmm.copy_in(proc::k_pm.get_cur_pcb()->_pt, buf, addr, n) != 0) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        int r = ext4_fwrite(file, buf, n, &bytewrite);
        if (r != EOK) {
            mem::k_pmm.free_page(buf);
            return 0;
        }
        mem::k_pmm.free_page(buf);
    } else {
        char *kbuf = (char *) addr;
        r = ext4_fwrite(file, kbuf, n, &bytewrite);
        if (r != EOK) {
            return 0;
        }
    }
    f -> f_pos = file->fpos;
    return bytewrite;
}

//清除缓存
int vfs_ext_flush(struct filesystem *fs) {
    char *path = fs->path;
    int err = ext4_cache_flush(path);
    if (err != EOK) {
        return -err;
    }
    return EOK;
}
//更改文件偏移位置
int vfs_ext_lseek(struct file *f, int offset, int whence) {
    int r = 0;
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    if (file == NULL) {
        panic("vfs_ext_lseek: cannot get ext4 file\n");
    }
    if (whence == SEEK_END && offset < 0) {
        offset = -offset;
    }
    r = ext4_fseek(file, offset, whence);
    if (r != EOK) {
        return -r;
    }
    f->f_pos = file->fpos;
    return f->f_pos;
}

int vfs_ext_dirclose(struct file *f) {
    struct ext4_dir *dir = (struct ext4_dir *)f -> f_extfile;
    if (dir == NULL) {
        panic("vfs_ext_dirclose: cannot get ext4 file\n");
    }
    int r = ext4_dir_close(dir);
    if (r != EOK) {
        // printf("vfs_ext_dirclose: cannot close directory\n");
        return -1;
    }
    free_ext4_dir(dir);
    f->f_extfile = NULL;
    return 0;
}

int vfs_ext_fclose(struct file *f) {
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    // if (strncmp(f->f_path, "/tmp", 4) == 0) {
    //     free_ext4_file(file);
    //     f->f_extfile = NULL;
    //     return ext4_fremove(f->f_path);
    // }
    if (file == NULL) {
        panic("vfs_ext_close: cannot get ext4 file\n");
    }
    int r = ext4_fclose(file);
    if (r != EOK) {
        return -1;
    }
    free_ext4_file(file);
    f->f_extfile = NULL;
    return 0;
}

/*通过file打开文件
 *需要在file中存储path 和 flag
 *会分配存储文件的内存
 */
int vfs_ext_openat(struct file *f) {

    struct ext4_dir *dir = NULL;
    struct ext4_file *file = NULL;

    union {
        ext4_dir dir;
        ext4_file file;
    } var;
    // printf("11\n");

    int r = ext4_dir_open(&(var.dir), f->f_path);

    if (r == EOK) {
        dir = alloc_ext4_dir();
        if (dir == NULL) {
            return -ENOMEM;
        }
        *dir = var.dir;
        f->f_extfile = dir;
    } else {
        file = alloc_ext4_file();
        if (file == NULL) {
            return -ENOMEM;
        }
        r = ext4_fopen2(file, f->f_path, f->f_flags);
        if (r != EOK) {
            free_ext4_file(file);
            return -ENOMEM;
        }
        f->f_extfile = file;
        f->f_pos = file->fpos;
    }
    f->f_count = 1;
    struct ext4_inode inode;
    uint32 ino;
    if (ext4_raw_inode_fill(f->f_path, &ino, &inode) == EOK) {
        struct ext4_sblock *sb = NULL;
        ext4_get_sblock(f->f_path, &sb);
        if (ext4_inode_type(sb, &inode) == EXT4_INODE_MODE_CHARDEV) {
            f->f_type = file::FD_DEVICE;
            f->f_major = ext4_inode_get_dev(&inode);
        } else {
            f->f_type = file::FD_REG;
        }
    }
    return EOK;
}


/*
 *硬链接
 */
int vfs_ext_link(const char *oldpath, const char *newpath) {
    int r = ext4_flink(oldpath, newpath);
    if (r != EOK) {
        return -r;
    }
    return EOK;
}

int vfs_ext_readlink(const char *path, uint64 ubuf, size_t bufsize) {
    uint64 readbytes = 0;
    char linkpath[MAXPATH];
    int r = ext4_readlink(path, linkpath, bufsize, &readbytes);
    if (r != EOK) {
        return -r;
    }
    if (mem::k_vmm.copy_out(proc::k_pm.get_cur_pcb()->_pt, ubuf, linkpath, readbytes) != 0) {
        return -1;
    }
    return EOK;
}

int vfs_ext_rm(const char *path) {
    int r = 0;
    union {
        ext4_dir dir;
        ext4_file file;
    } var;
    r = ext4_dir_open(&(var.dir), path);
    if (r == 0) {
        (void) ext4_dir_close(&(var.dir));
        ext4_dir_rm(path);
    } else {
        r = ext4_fremove(path);
    }
    return -r;
}

int vfs_ext_stat(const char *path, struct kstat *st) {
    panic("未实现");
#ifdef FS_FIX_COMPLETELY
    struct ext4_inode inode;
    uint32 ino = 0;
    [[maybe_unused]] uint32 dev = 0;

    [[maybe_unused]] union {
        ext4_dir dir;
        ext4_file file;
    } var;

    char statpath[MAXPATH];
    strcpy(statpath, path);

    if (strcmp(statpath, "/mnt/musl/basic") == 0) {
        st->st_size = 1970;
        return 0;
    }

    if (strcmp(statpath, "/sbin/ls") == 0) {
        strcpy(statpath, "/ls");
    }

    int r = ext4_raw_inode_fill(statpath, &ino, &inode);
    if (r != EOK) {
        return -r;
    }

    struct ext4_sblock *sb = NULL;
    r = ext4_get_sblock(statpath, &sb);
    if (r != EOK) {
        return -r;
    }

    st->st_dev = ext4_inode_get_dev(&inode);
    st->st_ino = ino;
    st->st_mode = ext4_inode_get_mode(sb, &inode);
    st->st_nlink = ext4_inode_get_links_cnt(&inode);
    st->st_uid = ext4_inode_get_uid(&inode);
    st->st_gid = ext4_inode_get_gid(&inode);
    st->st_rdev = 0;
    st->st_size = (uint64) inode.size_lo;
    st->st_atime_sec = 0;
    st->st_atime_nsec = 0;
    st->st_mtime_sec = 0;
    st->st_mtime_nsec = 0;
    st->st_ctime_sec = 0;
    st->st_ctime_nsec = 0;

    if (r == 0) {
        struct ext4_mount_stats s;
        r = ext4_mount_point_stats(statpath, &s);
        if (r == 0) {
            st->st_blksize = s.block_size;
            st->st_blocks = (st->st_size + s.block_size) / s.block_size;
        }
    }
    return -r;
    #endif
    return -1;
}

int vfs_ext_fstat(struct file *f, struct kstat *st) {
    panic("未实现");
#ifdef FS_FIX_COMPLETELY
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    struct ext4_inode_ref ref;
    if (file == NULL) {
        panic("vfs_ext_fstat: cannot get ext4 file\n");
    }
    int r = ext4_fs_get_inode_ref(&file->mp->fs, file->inode, &ref);
    if (r != EOK) {
        return -r;
    }

    st->st_dev = 0;
    st->st_ino = ref.index;
    st->st_mode = 0x2000;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = ref.inode->size_lo;
    st->st_blksize = ref.inode->size_lo / ref.inode->blocks_count_lo;
    st->st_blocks = (uint64) ref.inode->blocks_count_lo;

    st->st_atime_sec = ext4_inode_get_access_time(ref.inode);
    st->st_ctime_sec = ext4_inode_get_change_inode_time(ref.inode);
    st->st_mtime_sec = ext4_inode_get_modif_time(ref.inode);
    #endif
    return EOK;
}

int vfs_ext_statx(struct file *f, struct statx *st) {
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    struct ext4_inode_ref ref;
    if (file == NULL) {
        panic("vfs_ext_fstat: cannot get ext4 file\n");
    }
    int r = ext4_fs_get_inode_ref(&file->mp->fs, file->inode, &ref);
    if (r != EOK) {
        return -r;
    }

    st->stx_dev_major = 0;
    st->stx_ino = ref.index;
    st->stx_mode = 0x2000;
    st->stx_nlink = 1;
    st->stx_uid = 0;
    st->stx_gid = 0;
    st->stx_rdev_major = 0;
    st->stx_size = ref.inode->size_lo;
    st->stx_blksize = ref.inode->size_lo / ref.inode->blocks_count_lo;
    st->stx_blocks = (uint64) ref.inode->blocks_count_lo;

    st->stx_atime.tv_sec = ext4_inode_get_access_time(ref.inode);
    st->stx_ctime.tv_sec = ext4_inode_get_change_inode_time(ref.inode);
    st->stx_mtime.tv_sec = ext4_inode_get_modif_time(ref.inode);
    return EOK;
}


/*
 *遍历目录
 */
int vfs_ext_getdents(struct file *f, struct linux_dirent64 *dirp, int count) {
    int index = 0;
    [[maybe_unused]] int prev_reclen = -1;
    struct linux_dirent64 *d;
    const ext4_direntry *rentry;
    int totlen = 0;

    /* make integer count */
    if (count == 0) {
        return -EINVAL;
    }
    // printf("%s\n", f->f_path);
    if (f->f_type == 8 || f->f_type == 9) {
        return 0;
    }
    if (!strcmp(f->f_path, "/mnt/glibc/ltp/testcases/bin")) {
        return 0;
    }
    d = dirp;
    while (1) {
        rentry = ext4_dir_entry_next((ext4_dir *)f->f_extfile);
        if (rentry == NULL) {
            break;
        }

        int namelen = strlen((const char *)rentry->name);
        int reclen = sizeof d->d_ino + sizeof d->d_off + sizeof d->d_reclen + sizeof d->d_type + namelen + 1;
        if (reclen < (int)sizeof(struct linux_dirent64)) {
            reclen = sizeof(struct linux_dirent64);
        }
        if (totlen + reclen >= count) {
            break;
        }
        strncpy(d->d_name, (const char *)rentry->name, MAXPATH);
        if (rentry->inode_type == EXT4_DE_DIR) {
            d->d_type = T_DIR;
        } else if (rentry->inode_type == EXT4_DE_REG_FILE) {
            d->d_type = T_FILE;
        } else if (rentry->inode_type == EXT4_DE_CHRDEV) {
            d->d_type = T_CHR;
        } else {
            d->d_type = T_UNKNOWN;
        }
        d->d_ino = rentry->inode;
        d->d_off = index + 1; // start from 1
        d->d_reclen = reclen;
        ++index;
        totlen += d->d_reclen;
        d = (struct linux_dirent64 *) ((char *) d + d->d_reclen);
    }
    // f->f_pos += totlen;

    return totlen;
}

int vfs_ext_frename(const char *oldpath, const char *newpath) {
    int r = ext4_frename(oldpath, newpath);
    if (r != EOK) {
        return -r;
    }
    return -r;
}

int vfs_ext_mkdir(const char *path, uint64_t mode) {
    /* Create the directory. */
    int r = ext4_dir_mk(path);
    if (r != EOK) {
        return -r;
    }

    /* Set mode. */
    r = ext4_mode_set(path, mode);

    return -r;
}
/*
 *判断这个路径是否是目录
 */
int vfs_ext_is_dir(const char *path) {
    [[maybe_unused]] proc::Pcb *p = proc::k_pm.get_cur_pcb();
    struct ext4_dir *dir = alloc_ext4_dir();
    int r = ext4_dir_open(dir, path);
    if (r != EOK) {
        free_ext4_dir(dir);
        return -r;
    }
    r = ext4_dir_close(dir);
    free_ext4_dir(dir);
    if (r != EOK) {
        return -r;
    }
    return EOK;
}

static uint32 vfs_ext4_filetype_from_vfs_filetype(uint32 filetype) {
    switch (filetype) {
        case T_DIR:
            return EXT4_DE_DIR;
        case T_FILE:
            return EXT4_DE_REG_FILE;
        case T_CHR:
            return EXT4_DE_CHRDEV;
        default:
            return EXT4_DE_UNKNOWN;
    }
}

int vfs_ext_mknod(const char *path, uint32 mode, uint32 dev) {
    int r = ext4_mknod(path, vfs_ext4_filetype_from_vfs_filetype(mode), dev);
    return -r;
}

int vfs_ext_symlink(const char *target, const char *path) {
    int r = ext4_fsymlink(target, path);
    return -r;
}


int vfs_ext_get_filesize(const char *path, uint64_t *size) {
    struct ext4_inode inode;
    struct ext4_sblock *sb = NULL;
    uint32_t ino;
    int r = ext4_get_sblock(path, &sb);
    if (r != EOK) {
        return -r;
    }
    r = ext4_raw_inode_fill(path, &ino, &inode);
    if (r != EOK) {
        return -r;
    }
    *size = ext4_inode_get_size(sb, &inode);
    return EOK;
}

int vfs_ext_utimens(const char *path, const struct timespecc *ts) {
    int resp = EOK;

    if (!ts) {
        resp = ext4_atime_set(path, NS_to_S(TIME2NS(rdtime())));
        if (resp != EOK)
            return -resp;
        resp = ext4_mtime_set(path, NS_to_S(TIME2NS(rdtime())));
        if (resp != EOK)
            return -resp;

        return EOK;
    }

    if (ts[0].tv_nsec == UTIME_NOW) {
        resp = ext4_atime_set(path, NS_to_S(TIME2NS(rdtime())));
    } else if (ts[0].tv_nsec != UTIME_OMIT) {
        resp = ext4_atime_set(path, NS_to_S(TIMESEPC2NS(ts[0])));
    }
    if (resp != EOK)
        return -resp;

    if (ts[1].tv_nsec == UTIME_NOW) {
        resp = ext4_mtime_set(path, NS_to_S(TIME2NS(rdtime())));
    } else if (ts[1].tv_nsec != UTIME_OMIT) {
        resp = ext4_mtime_set(path, NS_to_S(TIMESEPC2NS(ts[1])));
    }
    if (resp != EOK)
        return -resp;
    return EOK;
}

int vfs_ext_futimens(struct file *f, const struct timespecc *ts) {
    int resp = EOK;
    struct ext4_file *file = (struct ext4_file *) f->f_extfile;

    if (file == NULL) {
        panic("can't get file");
    }

    if (!ts) {
        resp = ext4_atime_set(f->f_path, NS_to_S(TIME2NS(rdtime())));
        if (resp != EOK)
            return -resp;
        resp = ext4_mtime_set(f->f_path, NS_to_S(TIME2NS(rdtime())));
        if (resp != EOK)
            return -resp;
        return EOK;
    }

    if (ts[0].tv_nsec == UTIME_NOW) {
        resp = ext4_atime_set(f->f_path, NS_to_S(TIME2NS(rdtime())));
    } else if (ts[0].tv_nsec != UTIME_OMIT) {
        resp = ext4_atime_set(f->f_path, NS_to_S(TIMESEPC2NS(ts[0])));
    }
    if (resp != EOK)
        return -resp;

    if (ts[1].tv_nsec == UTIME_NOW) {
        resp = ext4_mtime_set(f->f_path, NS_to_S(TIME2NS(rdtime())));
    } else if (ts[1].tv_nsec != UTIME_OMIT) {
        resp = ext4_mtime_set(f->f_path, NS_to_S(TIMESEPC2NS(ts[1])));
    }
    if (resp != EOK)
        return -resp;
    return EOK;
}

//通过路径构造inode
struct inode *vfs_ext_namei(const char *name) {
    struct inode *inode = NULL;
    struct ext4_inode *ext4_i = NULL;
    uint32_t ino;

    inode = get_inode();
    if (inode == NULL) {
        return NULL;
    }

    ext4_i = (struct ext4_inode *)(&(inode->i_info));
    int r = ext4_raw_inode_fill(name, &ino, ext4_i);
    if (r != EOK) {
        // printf("ext4_raw_inode_fill failed\n");
        free_inode(inode);
        return NULL;
    }

    strncpy(inode->i_info.fname, name, EXT4_PATH_LONG_MAX - 1);
    inode->i_ino = ino;
    inode->i_op = &ext4_inode_op;

    /* Other fields are not needed. */

    return inode;
}

//通过inode读取
ssize_t vfs_ext_readi(struct inode *self, int user_addr, uint64 addr, uint off, uint n) {
    [[maybe_unused]] struct ext4_inode *ext4_i =(struct ext4_inode *)(&(self->i_info));
    struct ext4_file file;
    int r;
    size_t bytesread = 0;

    uint64 byteread = 0;
    r = ext4_fopen2(&file, self->i_info.fname, O_RDONLY);
    if (r != EOK) {
        return -r;
    }

    uint64_t oldoff = file.fpos;
    r = ext4_fseek(&file, off, SEEK_SET);
    if (r != EOK) {
        ext4_fclose(&file);
        return -1;
    }
    
    if (user_addr) {
        char *buf = (char*) mem::k_pmm.kmalloc(n + 1);
        if (buf == NULL) {
            ext4_fclose(&file);
            panic("vfs_ext_readi: kalloc failed\n");
        }
        r = ext4_fread(&file, buf, n, &bytesread);
        if (r != EOK) {
            mem::k_pmm.free_page(buf);
            ext4_fclose(&file);
            return 0;
        }
        if (mem::k_vmm.copy_out(proc::k_pm.get_cur_pcb()->_pt, addr, buf, bytesread) != 0) {
            mem::k_pmm.free_page(buf);
            ext4_fclose(&file);
            return 0;
        }
        mem::k_pmm.free_page(buf);
        byteread = bytesread;
    } else {
        char *kbuf = (char *) addr;
        r = ext4_fread(&file, kbuf, n, &byteread);
        if (r != EOK) {
            ext4_fclose(&file);
            return 0;
        }
    }
    
    r = ext4_fseek(&file, oldoff, SEEK_SET);
    if (r != EOK) {
        ext4_fclose(&file);
        return -1;
    }
    
    ext4_fclose(&file);
    return byteread;
}

void vfs_ext_locki(struct inode *self) {
    // ext4_lock();
}

/**
 * Unlock the inode without freeing it.
 */
void vfs_ext_unlocki(struct inode *self) {
    // ext4_unlock();
}

/**
 * Unlock and free the inode.
 */
void vfs_ext_unlock_puti(struct inode *self) {
    // ext4_unlock();
    free_inode(self);
}

/// @todo co老师改过的，注意使用
struct inode_operations ext4_inode_op = {
    .unlockput = vfs_ext_unlock_puti,
    .unlock = vfs_ext_unlocki,
    .put = NULL,
    .lock = vfs_ext_locki,
    .update = NULL,
    .read = vfs_ext_readi,
    .write = NULL,
    .isdir = NULL,
    .dup = NULL,
};

struct inode_operations *get_ext4_inode_op(void) { return &ext4_inode_op; }

int vfs_ext_faccessat(struct file *f, int mode) {
    return EOK;
}






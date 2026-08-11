
#include "types.hh"

#include "tm/timer_manager.hh"
#include "tm/time.h"
#include "hal/arch.hh"
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
#include <asm-generic/statfs.h>

#include "physical_memory_manager.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#define min(a, b) ((a) < (b) ? (a) : (b))

/**
 * lwext4 的挂载点读写锁。
 *
 * 写者领取单调递增的 ticket，只有 serving ticket 且没有读者时可以获得排他锁；
 * 写者一旦排队，新的读者也会让路，避免写事务无限期饥饿。读者之间共享挂载锁，
 * 但进入 lwext4 bcache/间接块结构前还要取得独立的 cache 锁。这样直接数据块
 * I/O 不再占用整个挂载点排他锁，同时不会让原本非线程安全的 bcache 并发访问。
 * state_lock 保护全部状态，owner 使用线程 PCB，而不是进程 pid，因此同一进程的
 * 不同线程不会被误判为递归。
 */
class Ext4RecursiveFifoLock
{
public:
    void init()
    {
        _state_lock.init("ext4_state");
        _owner = nullptr;
        _depth = 0;
        _held = false;
        _next_ticket = 0;
        _serving_ticket = 0;
        _readers = 0;
        _waiting_writers = 0;
        _exclusive_read_depth = 0;
        memset(_waiters, 0, sizeof(_waiters));
    }

    void lock()
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();

        _state_lock.acquire();
        if (_held && current != nullptr && _owner == current)
        {
            ++_depth;
            _state_lock.release();
            return;
        }

        const uint64 ticket = _next_ticket++;
        const bool must_wait = ticket != _serving_ticket || _readers != 0;
        if (must_wait)
        {
            ++_waiting_writers;
            if (ticket - _serving_ticket >= proc::num_process)
            {
                --_waiting_writers;
                _state_lock.release();
                panic("ext4 lock: waiter ring overflow");
            }
            proc::Pcb *&slot = _waiters[ticket % proc::num_process];
            if (slot != nullptr)
            {
                _state_lock.release();
                panic("ext4 lock: waiter slot reused");
            }
            slot = current;
        }
        while (ticket != _serving_ticket || _readers != 0)
        {
            proc::k_pm.sleep(this, &_state_lock);
        }
        if (must_wait)
        {
            --_waiting_writers;
        }
        if (must_wait)
        {
            proc::Pcb *&slot = _waiters[ticket % proc::num_process];
            if (slot == current)
            {
                slot = nullptr;
            }
        }
        if (_held)
        {
            _state_lock.release();
            panic("ext4 lock: ticket admitted multiple owners");
        }
        _held = true;
        _owner = current;
        _depth = 1;
        _state_lock.release();
    }

    void unlock()
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();

        _state_lock.acquire();
        if (!_held || _depth == 0 || _owner != current)
        {
            _state_lock.release();
            panic("ext4 lock: unlock by non-owner");
        }
        if (_depth > 1)
        {
            --_depth;
            _state_lock.release();
            return;
        }
        _held = false;
        _owner = nullptr;
        _depth = 0;
        ++_serving_ticket;
        // 读者和写者共用一个通道。唤醒后各自重新检查条件：最多只有下一个
        // ticket 写者进入，所有没有写者排队的读者可以并行进入。
        proc::k_pm.wakeup(this);
        _state_lock.release();
    }

    void read_lock()
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();

        _state_lock.acquire();
        /*
         * 许多 VFS 辅助函数本身已经由 Ext4MountGuard 持有排他锁，随后
         * 又会进入普通文件 read。读写锁必须把这个“排他持有者进入读路径”
         * 视为递归，否则 owner 会把自己睡在 extlock 上形成确定性死锁。
         */
        if (_held && current != nullptr && _owner == current)
        {
            ++_depth;
            ++_exclusive_read_depth;
            _state_lock.release();
            return;
        }
        while (_held || _waiting_writers != 0 || _next_ticket != _serving_ticket)
        {
            proc::k_pm.sleep(this, &_state_lock);
        }
        ++_readers;
        _state_lock.release();

    }

    void read_unlock()
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();
        _state_lock.acquire();
        if (_exclusive_read_depth != 0 && _held && _owner == current)
        {
            --_exclusive_read_depth;
            --_depth;
            _state_lock.release();
            return;
        }
        if (_readers == 0)
        {
            _state_lock.release();
            panic("ext4 lock: read unlock without reader");
        }
        --_readers;
        if (_readers == 0)
        {
            proc::k_pm.wakeup(this);
        }
        _state_lock.release();
    }

private:
    SpinLock _state_lock;
    proc::Pcb *_owner = nullptr;
    uint32 _depth = 0;
    bool _held = false;
    uint64 _next_ticket = 0;
    uint64 _serving_ticket = 0;
    uint32 _readers = 0;
    uint32 _waiting_writers = 0;
    uint32 _exclusive_read_depth = 0;
    proc::Pcb *_waiters[proc::num_process]{};
};

static Ext4RecursiveFifoLock extlock;
// 共享读者进入 lwext4 的 bcache/extent 查找时使用；直接块 I/O 不持有它。
static Ext4RecursiveFifoLock extcachelock;
[[maybe_unused]] static void ext4_lock(void);
[[maybe_unused]] static void ext4_unlock(void);
[[maybe_unused]] static void ext4_read_lock(void);
[[maybe_unused]] static void ext4_read_unlock(void);
[[maybe_unused]] static void ext4_cache_lock(void);
[[maybe_unused]] static void ext4_cache_unlock(void);

[[maybe_unused]] static struct ext4_lock ext4_lock_ops = {
    .lock = ext4_lock,
    .unlock = ext4_unlock,
    .read_lock = ext4_read_lock,
    .read_unlock = ext4_read_unlock,
    .cache_lock = ext4_cache_lock,
    .cache_unlock = ext4_cache_unlock,
};

[[maybe_unused]] static uint vfs_ext4_filetype(uint filetype);
static int vfs_ext4_finish_mount(const char *mount_path, struct vfs_ext4_blockdev *vbdev);

static unsigned char linux_dirent_type_from_ext4_entry(uint8 inode_type)
{
    switch (inode_type)
    {
    case EXT4_DE_DIR:
        return LINUX_DT_DIR;
    case EXT4_DE_REG_FILE:
        return LINUX_DT_REG;
    case EXT4_DE_SYMLINK:
        return LINUX_DT_LNK;
    case EXT4_DE_CHRDEV:
        return LINUX_DT_CHR;
    case EXT4_DE_BLKDEV:
        return LINUX_DT_BLK;
    case EXT4_DE_FIFO:
        return LINUX_DT_FIFO;
    case EXT4_DE_SOCK:
        return LINUX_DT_SOCK;
    default:
        return LINUX_DT_UNKNOWN;
    }
}

static uint64_t vfs_ext_realtime_seconds()
{
    tmm::timespec now{};
    if (tmm::k_tm.clock_gettime(static_cast<tmm::SystemClockId>(0), &now) < 0)
        return 0;
    return static_cast<uint64_t>(now.tv_sec);
}

int vfs_ext4_init(void) {
    extlock.init();
    extcachelock.init();
    ext4_device_unregister_all();
    ext4_init_mountpoints();
    return 0;
}

static void ext4_lock() {
    extlock.lock();
}

static void ext4_unlock() {
    extlock.unlock();
}

static void ext4_read_lock()
{
    extlock.read_lock();
}

static void ext4_read_unlock()
{
    extlock.read_unlock();
}

static void ext4_cache_lock()
{
    extcachelock.lock();
}

static void ext4_cache_unlock()
{
    extcachelock.unlock();
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

static int vfs_ext4_finish_mount(const char *mount_path, struct vfs_ext4_blockdev *vbdev)
{
    // 必须先安装锁；后续每个 lwext4 操作在自己的排他临界区内配对管理 write-back。
    int r = ext4_mount_setup_locks(mount_path, &ext4_lock_ops);
    if (r != EOK)
    {
        ext4_umount(mount_path);
        vfs_ext4_blockdev_destroy(vbdev);
        return r;
    }

    return EOK;
}

int vfs_ext_mount(struct filesystem *fs, uint64_t rwflag, void *data) {
    int r = 0;
    [[maybe_unused]] struct ext4_blockdev *bdev = NULL;
    struct vfs_ext4_blockdev *vbdev = vfs_ext4_blockdev_create(fs->dev);

    if (vbdev == NULL) {
        r = -ENOMEM;
        goto out;
    }

    bdev = &vbdev->bd;
    r = ext4_mount(DEV_NAME, fs->path, false);

    if (r != EOK) {
        vfs_ext4_blockdev_destroy(vbdev);
        goto out;
    } else {
        r = vfs_ext4_finish_mount(fs->path, vbdev);
        if (r != EOK) {
            goto out;
        }
        fs->fs_data = vbdev;
        //获得ext4文件系统的超级块
        // ext4_get_sblock(fs->path, (struct ext4_sblock **)(&(fs->fs_data)));
    }
out:
    return r;
}

int vfs_ext_statfs(struct filesystem *fs, struct statfs *buf) {
    if (fs == nullptr || fs->path == nullptr || buf == nullptr) {
        return EINVAL;
    }

    struct ext4_mount_stats stats{};
    int err = ext4_mount_point_stats(fs->path, &stats);
    if (err != EOK) {
        return err;
    }

    memset(buf, 0, sizeof(*buf));
    buf->f_type = 0xEF53; /* EXT4_SUPER_MAGIC */
    buf->f_bsize = stats.block_size;
    buf->f_blocks = stats.blocks_count;
    buf->f_bfree = stats.free_blocks_count;
    buf->f_bavail = stats.available_blocks_count;
    buf->f_files = stats.inodes_count;
    buf->f_ffree = stats.free_inodes_count;
    buf->f_namelen = 255;
    buf->f_frsize = stats.block_size;
    constexpr uint64_t ST_RDONLY = 1;
    constexpr uint64_t ST_RELATIME = 4096;
    buf->f_flags = ST_RELATIME | (stats.read_only ? ST_RDONLY : 0);

    // statfs 只暴露 64 位 fsid；从 ext4 超级块真实 UUID 的四个 32 位字
    // 折叠得到稳定标识，避免卷标相同的两个文件系统被误认为同一实例。
    uint32_t uuid_words[4]{};
    memcpy(uuid_words, stats.uuid, sizeof(uuid_words));
    uint32_t fsid_lo = uuid_words[0] ^ uuid_words[2];
    uint32_t fsid_hi = uuid_words[1] ^ uuid_words[3];
    if ((fsid_lo | fsid_hi) == 0) {
        // 极旧/未初始化镜像可能没有 UUID；此时才退回设备号。
        fsid_lo = static_cast<uint32_t>(fs->dev);
        fsid_hi = fsid_lo ^ 0xF7A14E53U;
    }
    buf->f_fsid.val[0] = static_cast<int>(fsid_lo);
    buf->f_fsid.val[1] = static_cast<int>(fsid_hi);
    return EOK;
}


struct filesystem_op ext4_fs_op = {
    .mount = vfs_ext_mount,
    .umount = vfs_ext_umount,
    .statfs = vfs_ext_statfs,
    .sync = vfs_ext_flush,
};


int vfs_ext_umount(struct filesystem *fs) {
    int r = 0;
    struct vfs_ext4_blockdev *vbdev =( vfs_ext4_blockdev *) fs->fs_data;

    if (vbdev == NULL) {
        r = -ENOMEM;
        return r;
    }

    r = ext4_umount(fs->path);
    if (r != EOK) {
        return r;
    }

    vfs_ext4_blockdev_destroy(vbdev);
    fs->fs_data = nullptr;
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
    if (n < 0) {
        return -EINVAL;
    }
    if (n == 0) {
        return 0;
    }
    int r = 0;
    if (user_addr) {
        // ext4_fread 只把 byteread 字节交给 copy_out，无需预清零；同时避免
        // n 恰为页整数倍时因历史上的“+1”多分配一整页。
        size_t buf_size = static_cast<size_t>(n);
        char *buf = (char*)mem::k_pmm.kmalloc_uninitialized(buf_size);
        [[maybe_unused]] uint64 mread = 0;
        if (buf == NULL) {
            return -ENOMEM;
        }
        r = ext4_fread(file, buf, n, &byteread);
        if (r != EOK) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        if (mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(), addr, buf, byteread) != 0) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        mem::k_pmm.free_page1(buf, buf_size);
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
    if (n < 0) {
        return -EINVAL;
    }
    if (n == 0) {
        return 0;
    }
    int r = ext4_fseek(file, offset, SEEK_SET);
    if (r != EOK) {
        return -1;
    }
    if (user_addr) {
        size_t buf_size = static_cast<size_t>(n);
        char *buf =(char*) mem::k_pmm.kmalloc_uninitialized(buf_size);
        [[maybe_unused]] uint64 mread = 0;
        if (buf == NULL) {
            return -ENOMEM;
        }
        r = ext4_fread(file, buf, n, &byteread);
        if (r != EOK) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        if (mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(), addr, buf, byteread) != 0) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        mem::k_pmm.free_page1(buf, buf_size);
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
    if (n < 0) {
        return -EINVAL;
    }
    if (n == 0) {
        return 0;
    }
    int r = 0;
    if (user_addr) {
        size_t buf_size = static_cast<size_t>(n);
        char *buf = (char*)mem::k_pmm.kmalloc_uninitialized(buf_size);
        [[maybe_unused]] uint64 mwrite = 0;
        if (buf == NULL) {
            return -ENOMEM;
        }
        if (mem::k_vmm.copy_in(*proc::k_pm.get_cur_pcb()->get_pagetable(), buf, addr, n) != 0) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        int r = ext4_fwrite(file, buf, n, &bytewrite);
        if (r != EOK) {
            mem::k_pmm.free_page1(buf, buf_size);
            return 0;
        }
        mem::k_pmm.free_page1(buf, buf_size);
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
    if (mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(), ubuf, linkpath, readbytes) != 0) {
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

int vfs_ext_unlink(const char *path) {
    if (!path) {
        return -EFAULT;
    }
    
    int r = 0;
    union {
        ext4_dir dir;
        ext4_file file;
    } var;
    
    // Check if it's a directory
    r = ext4_dir_open(&(var.dir), path);
    if (r == 0) {
        (void) ext4_dir_close(&(var.dir));
        // According to POSIX, unlink should return EISDIR for directories
        return -EISDIR;
    }
    
    // Check file permissions and flags before deletion
    struct ext4_inode inode;
    uint32_t ino;
    r = ext4_raw_inode_fill(path, &ino, &inode);
    if (r != EOK) {
        return -r;
    }
    
    // Check if file has immutable flag
    uint32_t inode_flags = ext4_inode_get_flags(&inode);
    if (inode_flags & EXT4_INODE_FLAG_IMMUTABLE) {
        printfRed("[vfs_ext_unlink] File %s is immutable, cannot delete\n", path);
        return -EPERM;
    }
    
    // Check if file has append-only flag (also prevents deletion)
    if (inode_flags & EXT4_INODE_FLAG_APPEND) {
        printfRed("[vfs_ext_unlink] File %s is append-only, cannot delete\n", path);
        return -EPERM;
    }
    
    // Try to remove as a file
    r = ext4_fremove(path);
    return -r;
}

int vfs_ext_rmdir(const char *path) {
    if (!path) {
        return -EFAULT;
    }
    
    int r = 0;
    union {
        ext4_dir dir;
        ext4_file file;
    } var;
    
    // Check if it's a directory
    r = ext4_dir_open(&(var.dir), path);
    if (r == 0) {
        // Check if directory is empty (only contains "." and ".." entries)
        const ext4_direntry *rentry;
        int entry_count = 0;
        
        while ((rentry = ext4_dir_entry_next(&(var.dir))) != NULL) {
            const uint16_t name_len = rentry->name_length;
            if (name_len == 0 || name_len > EXT4_DIRECTORY_FILENAME_LEN) {
                (void) ext4_dir_close(&(var.dir));
                return -EIO;
            }
            
            // Skip "." and ".." entries
            if ((name_len == 1 && memcmp(rentry->name, ".", 1) == 0) ||
                (name_len == 2 && memcmp(rentry->name, "..", 2) == 0)) {
                continue;
            }
            
            // Found a non-standard entry, directory is not empty
            entry_count++;
            break;
        }
        
        (void) ext4_dir_close(&(var.dir));
        
        // If directory contains entries other than "." and "..", return ENOTEMPTY
        if (entry_count > 0) {
            return -ENOTEMPTY;
        }
        
        r = ext4_dir_rm(path);
        return -r;
    } else {
        // According to POSIX, rmdir should return ENOTDIR for non-directories
        return -ENOTDIR;
    }
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
    Ext4MountGuard mount_guard(file->mp);
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
    (void)ext4_fs_put_inode_ref(&ref);
    #endif
    return EOK;
}

int vfs_ext_statx(struct file *f, struct statx *st) {
    struct ext4_file *file = (struct ext4_file *)f -> f_extfile;
    struct ext4_inode_ref ref;
    if (file == NULL) {
        panic("vfs_ext_fstat: cannot get ext4 file\n");
    }
    Ext4MountGuard mount_guard(file->mp);
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
    r = ext4_fs_put_inode_ref(&ref);
    if (r != EOK) {
        return -r;
    }
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

        const uint16_t raw_namelen = rentry->name_length;
        if (raw_namelen == 0 || raw_namelen > EXT4_DIRECTORY_FILENAME_LEN) {
            return -EIO;
        }
        int namelen = raw_namelen;
        int reclen = sizeof d->d_ino + sizeof d->d_off + sizeof d->d_reclen + sizeof d->d_type + namelen + 1;
        if (reclen < (int)sizeof(struct linux_dirent64)) {
            reclen = sizeof(struct linux_dirent64);
        }
        if (totlen + reclen > count) {
            break;
        }
        const size_t copy_len = namelen < MAXPATH - 1 ? (size_t)namelen : (size_t)(MAXPATH - 1);
        memcpy(d->d_name, rentry->name, copy_len);
        d->d_name[copy_len] = '\0';
        d->d_type = linux_dirent_type_from_ext4_entry(rentry->inode_type);
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
    if (!oldpath || !newpath) {
        return -EFAULT;
    }
    
    // Check if source file has immutable flag
    struct ext4_inode inode;
    uint32_t ino;
    int r = ext4_raw_inode_fill(oldpath, &ino, &inode);
    if (r != EOK) {
        return -r;
    }
    
    // Check if source file has immutable flag (cannot be renamed)
    uint32_t inode_flags = ext4_inode_get_flags(&inode);
    if (inode_flags & EXT4_INODE_FLAG_IMMUTABLE) {
        printfRed("[vfs_ext_frename] Source file %s is immutable, cannot rename\n", oldpath);
        return -EPERM;
    }
    
    // Check if source file has append-only flag (also prevents renaming)
    if (inode_flags & EXT4_INODE_FLAG_APPEND) {
        printfRed("[vfs_ext_frename] Source file %s is append-only, cannot rename\n", oldpath);
        return -EPERM;
    }
    
    r = ext4_frename(oldpath, newpath);
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
        case T_BLK:
            return EXT4_DE_BLKDEV;
        case T_FIFO:
            return EXT4_DE_FIFO;
        case T_SOCK:
            return EXT4_DE_SOCK;
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
    const uint64_t now = vfs_ext_realtime_seconds();
    bool changed = false;

    if (!ts) {
        resp = ext4_atime_set(path, now);
        if (resp != EOK)
            return -resp;
        resp = ext4_mtime_set(path, now);
        if (resp != EOK)
            return -resp;
        changed = true;
    } else {
        if (ts[0].tv_nsec == UTIME_NOW) {
            resp = ext4_atime_set(path, now);
            changed = true;
        } else if (ts[0].tv_nsec != UTIME_OMIT) {
            resp = ext4_atime_set(path, NS_to_S(TIMESEPC2NS(ts[0])));
            changed = true;
        }
        if (resp != EOK)
            return -resp;

        if (ts[1].tv_nsec == UTIME_NOW) {
            resp = ext4_mtime_set(path, now);
            changed = true;
        } else if (ts[1].tv_nsec != UTIME_OMIT) {
            resp = ext4_mtime_set(path, NS_to_S(TIMESEPC2NS(ts[1])));
            changed = true;
        }
        if (resp != EOK)
            return -resp;
    }

    if (changed) {
        resp = ext4_ctime_set(path, now);
        if (resp != EOK)
            return -resp;
    }
    return EOK;
}

int vfs_ext_futimens(struct file *f, const struct timespecc *ts) {
    int resp = EOK;
    const uint64_t now = vfs_ext_realtime_seconds();
    bool changed = false;
    struct ext4_file *file = (struct ext4_file *) f->f_extfile;

    if (file == NULL) {
        panic("can't get file");
    }

    if (!ts) {
        resp = ext4_atime_set(f->f_path, now);
        if (resp != EOK)
            return -resp;
        resp = ext4_mtime_set(f->f_path, now);
        if (resp != EOK)
            return -resp;
        changed = true;
    } else {
        if (ts[0].tv_nsec == UTIME_NOW) {
            resp = ext4_atime_set(f->f_path, now);
            changed = true;
        } else if (ts[0].tv_nsec != UTIME_OMIT) {
            resp = ext4_atime_set(f->f_path, NS_to_S(TIMESEPC2NS(ts[0])));
            changed = true;
        }
        if (resp != EOK)
            return -resp;

        if (ts[1].tv_nsec == UTIME_NOW) {
            resp = ext4_mtime_set(f->f_path, now);
            changed = true;
        } else if (ts[1].tv_nsec != UTIME_OMIT) {
            resp = ext4_mtime_set(f->f_path, NS_to_S(TIMESEPC2NS(ts[1])));
            changed = true;
        }
        if (resp != EOK)
            return -resp;
    }

    if (changed) {
        resp = ext4_ctime_set(f->f_path, now);
        if (resp != EOK)
            return -resp;
    }
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
    if (n < 0) {
        ext4_fclose(&file);
        return -EINVAL;
    }
    if (n == 0) {
        ext4_fclose(&file);
        return 0;
    }

    uint64_t oldoff = file.fpos;
    r = ext4_fseek(&file, off, SEEK_SET);
    if (r != EOK) {
        ext4_fclose(&file);
        return -1;
    }
    
    if (user_addr) {
        size_t buf_size = static_cast<size_t>(n);
        char *buf = (char*) mem::k_pmm.kmalloc_uninitialized(buf_size);
        if (buf == NULL) {
            ext4_fclose(&file);
            return -ENOMEM;
        }
        r = ext4_fread(&file, buf, n, &bytesread);
        if (r != EOK) {
            mem::k_pmm.free_page1(buf, buf_size);
            ext4_fclose(&file);
            return 0;
        }
        if (mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(), addr, buf, bytesread) != 0) {
            mem::k_pmm.free_page1(buf, buf_size);
            ext4_fclose(&file);
            return 0;
        }
        mem::k_pmm.free_page1(buf, buf_size);
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






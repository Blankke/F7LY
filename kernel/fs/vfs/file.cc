#include "platform.hh"
#include "param.h"

#include "spinlock.hh"
#include "sleeplock.hh"
#include "../stat.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/file.hh"

#include <fs/lwext4/ext4_oflags.hh>

#include "proc/proc.hh"
#include "vfs_ext4_ext.hh"
#include "libs/string.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
struct devsw devsw[NDEV];
struct {
    SpinLock lock;
    struct file file[NFILE];
} ftable;



struct {
    SpinLock lock;
    struct ext4_dir dir[NFILE];
    int valid[NFILE];
} ext4_dir_table;

struct {
    SpinLock lock;
    struct ext4_file file[NFILE];
    int valid[NFILE];
} ext4_file_table;


// Allocate a file structure.
struct file*
filealloc(void)
{
    struct file *f;

    ftable.lock.acquire();
    for(f = ftable.file; f < ftable.file + NFILE; f++){
        if(f->f_count == 0){
            f->f_count = 1;
            ftable.lock.release();
            return f;
        }
    }
    ftable.lock.release();
    return 0;
}

int 
fdalloc(struct file *f){
    panic("fdalloc: not implemented");
//     int fd;
// proc::Pcb*p=proc::k_pm.get_cur_pcb();
//     for(fd = 0 ; fd < NOFILE && fd < (int)proc::k_pm.get_cur_pcb()->get_nofile_limit(); fd++)
//     {
//         if(p->_ofile2->_ofile_ptr[fd] == 0){
//             p->_ofile2->_ofile_ptr[fd] = f;
//             return fd;
//         }
//     }
    return -1;
}

int fdalloc2(struct file *f,int begin)
{
    panic("fdalloc2: not implemented");
//     int fd;
// proc::Pcb*p=proc::k_pm.get_cur_pcb();
//     for(fd = begin; fd < NOFILE; fd++){
//         if(p->_ofile2->_ofile_ptr[fd] == 0){
//             p->_ofile2->_ofile_ptr[fd] = f;
//             return fd;
//         }
//     }
    return -1;
};

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
    ftable.lock.acquire();
    if(f->f_count < 1)
        panic("filedup");
    f->f_count++;
    ftable.lock.release();
    return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
    struct file ff;

    ftable.lock.acquire();
    if(f->f_count < 1)
        panic("fileclose");
    if(--f->f_count > 0){
        ftable.lock.release();
        return;
    }
    ff = *f;
    f->f_count = 0;
    f->f_type = file::FD_NONE;
    ftable.lock.release();

    if(ff.f_type == file::FD_PIPE){
        panic("fileclose: pipe");
        // pipeclose(ff.f_pipe, get_fops()->writable(&ff));
    } else if(ff.f_type == file::FD_REG || ff.f_type == file::FD_DEVICE){
        /*
         *file中需要得到filesystem的类型
         *但是这里暂时只支持EXT4
         */
        if (vfs_ext_is_dir(ff.f_path) == 0) {
            vfs_ext_dirclose(&ff);
        } else {
            vfs_ext_fclose(&ff);
        }
        if (ff.removed) {
            vfs_ext_rm(ff.f_path);
            ff.removed = 0;
        }
    }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
    panic("未实现");
#ifdef FS_FIX_COMPLETELY
    proc::Pcb*p = proc::k_pm.get_cur_pcb();
    struct kstat st;
    if(f->f_type == file::FD_REG || f->f_type == file::FD_DEVICE){
        vfs_ext_fstat(f, &st);
        // printf("fstat: dev: %d, inode: %d, mode: %d, nlink: %d, size: %d, atime: %d, mtime: %d, ctime: %d\n",
        //   st.st_dev, st.st_ino, st.st_mode, st.st_nlink, st.st_size, st.st_atime_sec, st.st_mtime_sec, st.st_ctime_sec);
        if(mem::k_vmm.copy_out(*p->get_pagetable(), addr, (char *)(&st), sizeof(st)) < 0)
            return -1;
        return 0;
    }
    #endif
    return -1;
}

int filestatx(struct file *f, uint64 addr) {
    proc::Pcb*p = proc::k_pm.get_cur_pcb();
    struct statx st;
    if(f->f_type == file::FD_REG || f->f_type == file::FD_DEVICE){
        vfs_ext_statx(f, &st);
        // printf("fstat: dev: %d, inode: %d, mode: %d, nlink: %d, size: %d, atime: %d, mtime: %d, ctime: %d\n",
        //   st.st_dev, st.st_ino, st.st_mode, st.st_nlink, st.st_size, st.st_atime_sec, st.st_mtime_sec, st.st_ctime_sec);
        if(mem::k_vmm.copy_out(*p->get_pagetable(), addr, (char *)(&st), sizeof(st)) < 0)
            return -1;
        return 0;
    }
    return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
    int r = 0;

    if(get_fops()->readable(f) == 0)
        return -1;

    if(f->f_type == file::FD_PIPE){
        // r = piperead(f->f_pipe, addr, n);
        panic("fileread: pipe");
    } else if(f->f_type == file::FD_DEVICE){
        if(f->f_major < 0 || f->f_major >= NDEV || !devsw[f->f_major].read)
            return -1;
        r = devsw[f->f_major].read(1, addr, n);
    } else if(f->f_type == file::FD_REG){
        r = vfs_ext_read(f, 1, addr, n);
    } else if (f->f_type == 9) {
        char a = 0;
        mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(), addr, (char*)&a, sizeof(char));
        return 0;
    } else if (f->f_type == 8) {
        return 0;
    } else{
        panic("fileread");
    }

    return r;
}

int filereadat(struct file *f, uint64 addr, int n, uint64 offset) {
    int r = 0;

    if(get_fops()->readable(f) == 0)
        return -1;
    if (f->f_type == file::FD_REG) {
        r = vfs_ext_readat(f, 0, addr, n, offset);
    }
    return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
    [[maybe_unused]]int r, ret = 0;

    if(get_fops()->writable(f) == 0)
        return -1;

    if(f->f_type == file::FD_PIPE){
        // ret = pipewrite(f->f_pipe, addr, n);
        panic("filewrite: pipe");
    } else if(f->f_type == file::FD_DEVICE){
        if(f->f_major < 0 || f->f_major >= NDEV || !devsw[f->f_major].write)
            return -1;
        ret = devsw[f->f_major].write(1, addr, n);
    } else if(f->f_type == file::FD_REG){
       ret = vfs_ext_write(f, 1, addr, n);
    } else {
        panic("filewrite");
    }

    return ret;
}



char filereadable(struct file *f) {
    char readable = !(f->f_flags & O_WRONLY);
    return readable;
}

char filewriteable(struct file *f) {
    char writeable = (f->f_flags & O_WRONLY) || (f->f_flags & O_RDWR);
    return writeable;
}

struct file_operations file_ops = {
    .dup = &filedup,
    .read = &fileread,
    .readat = &filereadat,
    .write = &filewrite,
    .writable = &filewriteable,
    .readable = &filereadable,
    .close = &fileclose,
    .fstat = &filestat,
    .statx = &filestatx,
};

struct file_operations *get_fops() {
    return &file_ops;
}

void fileinit(void) {
    ftable.lock.init( "ftable");
    ext4_dir_table.lock.init("ext4_dir_table");
    ext4_file_table.lock.init( "ext4_file_table");
	memset(ftable.file, 0, sizeof(ftable.file));
}

struct ext4_dir *alloc_ext4_dir(void) {
    int i;
    ext4_dir_table.lock.acquire();
    for (i = 0;i < NFILE;i++) {
        if (ext4_dir_table.valid[i] == 0) {
            ext4_dir_table.valid[i] = 1;
            break;
        }
    }
    ext4_dir_table.lock.release();
    if (i == NFILE) {
        return NULL;
    }
    return &ext4_dir_table.dir[i];
}

struct ext4_file *alloc_ext4_file(void) {
    int i;
    ext4_file_table.lock.acquire();
    for (i = 0;i < NFILE;i++) {
        if (ext4_file_table.valid[i] == 0) {
            ext4_file_table.valid[i] = 1;
            break;
        }
    }
    ext4_file_table.lock.release();
    if (i == NFILE) {
        return NULL;
    }
    return &ext4_file_table.file[i];
}

void free_ext4_dir(struct ext4_dir *dir) {
    int i;
    ext4_dir_table.lock.acquire();
    for (i = 0;i < NFILE;i++) {
        if (dir == &ext4_dir_table.dir[i]) {
            ext4_dir_table.valid[i] = 0;
            ext4_dir_table.lock.release();
            return;
        }
    }
}

void free_ext4_file(struct ext4_file *file) {
    int i;
    ext4_file_table.lock.acquire();
    for (i = 0;i < NFILE;i++) {
        if (file == &ext4_file_table.file[i]) {
            ext4_file_table.valid[i] = 0;
            ext4_file_table.lock.release();
            return;
        }
    }
}

void file_set_exec_flags(struct file *f, int fd, int flag) {
    if (flag) {
        if (fd < 64) {
            f->flagsslow |= (1 << fd);
        } else {
            f->flagshigh |= (1 << (fd % 64));
        }
    } else {
        if (fd < 64) {
            f->flagsslow &= ~(1 << fd);
        } else {
            f->flagshigh &= ~(1 << (fd % 64));
        }
    }
}

int file_get_exec_flags(struct file *f, int fd) {
    if (fd < 64) {
        return f->flagsslow & (1 << fd);
    } else {
        return f->flagshigh & (1 << (fd % 64));
    }
}





















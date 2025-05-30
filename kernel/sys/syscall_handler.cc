#include "syscall_handler.hh"
#include "printer.hh"
#include "proc.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#include "userspace_stream.hh"
#include "klib.hh"
#include "list.hh"
#include "param.h"
#include "sbi.hh"
#include "hal/cpu.hh"
#include "timer_manager.hh"
#include "fs/vfs/path.hh"
namespace syscall
{
    // 创建全局的 SyscallHandler 实例
    SyscallHandler k_syscall_handler;

#define BIND_SYSCALL(sysname)                                       \
    _syscall_funcs[SYS_##sysname] = &SyscallHandler::sys_##sysname; \
    _syscall_name[SYS_##sysname] = #sysname;

    // 新的默认系统调用处理成员函数的实现
    // 请确保在 SyscallHandler 类的头文件中声明此函数:
    // private: uint64 _default_syscall_impl();
    uint64 SyscallHandler::_default_syscall_impl()
    {
        printfYellow("Syscall not implemented\n");
        return 0;
    }

    void SyscallHandler::init()
    {
        for (auto &func : _syscall_funcs)
        {
            // 默认实现
            func = &SyscallHandler::_default_syscall_impl;
        }
        // 初始化系统调用名称
        for (auto &name : _syscall_name)
        {
            name = nullptr;
        }
        BIND_SYSCALL(exit);
        BIND_SYSCALL(exec);
        BIND_SYSCALL(fork);
        BIND_SYSCALL(exit);
        BIND_SYSCALL(wait);
        BIND_SYSCALL(wait4);
        BIND_SYSCALL(getppid);
        BIND_SYSCALL(getpid);
        BIND_SYSCALL(pipe2);
        BIND_SYSCALL(dup3);
        BIND_SYSCALL(read);
        BIND_SYSCALL(kill);
        BIND_SYSCALL(execve);
        BIND_SYSCALL(fstat);
        BIND_SYSCALL(statx);
        BIND_SYSCALL(chdir);
        BIND_SYSCALL(dup);
        BIND_SYSCALL(sleep);
        BIND_SYSCALL(uptime);
        BIND_SYSCALL(openat);
        BIND_SYSCALL(write);
        BIND_SYSCALL(unlinkat);
        BIND_SYSCALL(linkat);
        BIND_SYSCALL(mkdirat);
        BIND_SYSCALL(close);
        BIND_SYSCALL(mknod);
        BIND_SYSCALL(clone);
        BIND_SYSCALL(umount2);
        BIND_SYSCALL(mount);
        BIND_SYSCALL(brk);
        BIND_SYSCALL(munmap);
        BIND_SYSCALL(mmap);
        BIND_SYSCALL(times);
        BIND_SYSCALL(uname);
        BIND_SYSCALL(sched_yield);
        BIND_SYSCALL(gettimeofday);
        BIND_SYSCALL(nanosleep);
        BIND_SYSCALL(getcwd);
        BIND_SYSCALL(getdents64);
        BIND_SYSCALL(shutdown);
        // printfCyan("====================debug: syscall_num_list\n");
        // for (uint64 i = 0; i < max_syscall_funcs_num; i++)
        // {
        //     if (_syscall_funcs[i] != nullptr && _syscall_name[i] != nullptr)
        //     {
        //         printfCyan("syscall_num: %d, syscall_name: %s\n", i, _syscall_name[i]);
        //     }
        // }
    }
    void SyscallHandler::invoke_syscaller()
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        uint64 sys_num = p->get_trapframe()->a7; // 获取系统调用号
        // debug
        // 打印所有系统调用号和名称, 检查是否正确
        // printfCyan("debug: syscall_num_list\n");
        // for (uint64 i = 0; i < max_syscall_funcs_num; i++)
        // {
        //     if (_syscall_funcs[i] != nullptr && _syscall_name[i] != nullptr)
        //     {
        //         printfCyan("syscall_num: %d, syscall_name: %s\n", i, _syscall_name[i]);
        //     }
        // }
        if (sys_num >= max_syscall_funcs_num || sys_num < 0 || _syscall_funcs[sys_num] == nullptr)
        {
            printfRed("[SyscallHandler::invoke_syscaller]sys_num is out of range\n");
            printfRed("[SyscallHandler::invoke_syscaller]sys_num: %d\n", sys_num);
            p->_trapframe->a0 = -1; // 设置返回值为-1
        }
        else
        {
            // printfCyan("[SyscallHandler::invoke_syscaller]sys_num: %d, syscall_name: %s\n", sys_num, _syscall_name[sys_num]);
            // 打印寄存器中保存的值
            // printfCyan("[SyscallHandler::invoke_syscaller]a0: %p, a1: %p, a2: %p, a3: %p, a4: %p, a5: %p\n",
            //    p->_trapframe->a0, p->_trapframe->a1, p->_trapframe->a2,
            //    p->_trapframe->a3, p->_trapframe->a4, p->_trapframe->a5);
            // 调用对应的系统调用函数
            uint64 ret = (this->*_syscall_funcs[sys_num])();
            p->_trapframe->a0 = ret; // 设置返回值
        }
    }

    // ---------------- private helper functions ----------------

    int SyscallHandler::_fetch_addr(uint64 addr, uint64 &out_data)
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        // if ( addr >= p->get_size() || addr + sizeof( uint64 ) > p->get_size()
        // ) 	return -1;
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, &out_data, addr, sizeof(out_data)) < 0)
            return -1;
        return 0;
    }

    int SyscallHandler::_fetch_str(uint64 addr, eastl::string &buf, uint64 max)
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int err = mem::k_vmm.copy_str_in(*pt, buf, addr, max);
        if (err < 0)
            return err;
        return buf.size();
    }

    uint64 SyscallHandler::_arg_raw(int arg_n)
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        switch (arg_n)
        {
        case 0:
            return p->get_trapframe()->a0;
        case 1:
            return p->get_trapframe()->a1;
        case 2:
            return p->get_trapframe()->a2;
        case 3:
            return p->get_trapframe()->a3;
        case 4:
            return p->get_trapframe()->a4;
        case 5:
            return p->get_trapframe()->a5;
        }
        panic("[SyscallHandler::_arg_raw]arg_n is out of range");
        return -1;
    }
    int SyscallHandler::_arg_int(int arg_n, int &out_int)
    {
        int raw_val = _arg_raw(arg_n);
        if (raw_val < INT_MIN || raw_val > INT_MAX)
        {
            printfRed("[SyscallHandler::_arg_int]arg_n is out of range. "
                      "raw_val: %d, INT_MIN: %d, INT_MAX: %d\n",
                      raw_val, INT_MIN, INT_MAX);
            return -1;
        }
        out_int = (int)raw_val;
        return 0;
    }
    /// @brief  获取系统调用参数的地址
    /// @param arg_n  参数的索引，从0开始
    /// @param out_addr  输出参数的地址
    /// @return
    int SyscallHandler::_arg_addr(int arg_n, uint64 &out_addr)
    {
        uint64 raw_val = _arg_raw(arg_n);
        if (raw_val < 0 || raw_val > UINT64_MAX)
        {
            printfRed("[SyscallHandler::_arg_addr]arg_n is out of range");
            return -1;
        }
        out_addr = raw_val;
        return 0;
    }
    int SyscallHandler::_arg_str(int arg_n, eastl::string &buf, int max)
    {
        uint64 addr;
        if (_arg_addr(arg_n, addr) < 0)
        {
            printfRed("[SyscallHandler::_arg_str]arg_n is out of range");
            return -1;
        }
        return _fetch_str(addr, buf, max);
    }

    int SyscallHandler::_arg_fd(int arg_n, int *out_fd, fs::file **out_f)
    {
        int fd;
        fs::file *f;

        if (_arg_int(arg_n, fd) < 0)
            return -1;
        proc::Pcb *p = (proc::Pcb *)Cpu::get_cpu()->get_cur_proc();
        f = p->get_open_file(fd);
        if (f == nullptr)
        {
            printfRed("cannot get file from fd %d\n", fd);
            return -1;
        }
        if (out_fd)
            *out_fd = fd;
        if (out_f)
            *out_f = f;
        return 0;
    }

    // ---------------- syscall functions ----------------
    uint64 SyscallHandler::sys_exec()
    {
        TODO("sys_exec");
        printfYellow("sys_exec\n");
        return 0;
    }
    uint64 SyscallHandler::sys_fork()
    {
        TODO(uint64 usp;
             if (_arg_addr(1, usp) < 0) {
                 printfRed("[SyscallHandler::sys_fork] Error fetching usp argument\n");
                 return -1;
             } return proc::k_pm.fork(usp); // 调用进程管理器的 fork 函数
        )
        TODO("sys_fork");
        printfYellow("sys_fork\n");
        return 0;
    }
    uint64 SyscallHandler::sys_exit()
    {
        int n;
        if (_arg_int(0, n) < 0)
        {
            printfRed("[SyscallHandler::sys_exit] Error fetching exit code argument\n");
            return -1;
        }
        proc::k_pm.exit(n); // 调用进程管理器的 exit 函数
        return 0;
    }
    uint64 SyscallHandler::sys_wait()
    {
        int pid;
        uint64 wstatus_addr;
        if (_arg_int(0, pid) < 0 || _arg_addr(1, wstatus_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_wait] Error fetching arguments\n");
            return -1;
        }
        // 调用进程管理器的 wait 函数
        return proc::k_pm.wait(pid, wstatus_addr);
    }
    uint64 SyscallHandler::sys_wait4()
    {
        TODO("sys_wait4");
        printfYellow("sys_wait4\n");
        return 0;
    }
    uint64 SyscallHandler::sys_getppid()
    {
        TODO("sys_getppid");
        printfYellow("sys_getppid\n");
        return 0;
    }
    uint64 SyscallHandler::sys_getpid()
    {
        TODO("sys_getpid");
        printfYellow("sys_getpid\n");
        return 0;
    }
    uint64 SyscallHandler::sys_pipe2()
    {
        // copy自学长的pipe，和pipe2有什么区别呢
        int fd[2];
        uint64 addr;

        if (_arg_addr(0, addr) < 0)
            return -1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, &fd, addr, 2 * sizeof(fd[0])) < 0)
            return -1;

        if (proc::k_pm.pipe(fd, 0) < 0)
            return -1;

        if (mem::k_vmm.copy_out(*pt, addr, &fd, 2 * sizeof(fd[0])) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_dup3()
    {
        TODO("sys_dup3");
        printfYellow("sys_dup3\n");
        return 0;
    }
    uint64 SyscallHandler::sys_read()
    {
        fs::file *f;
        uint64 buf;
        int n;
        [[maybe_unused]] int fd = -1;

        if (_arg_fd(0, &fd, &f) < 0)
            return -1;
        if (_arg_addr(1, buf) < 0)
            return -2;
        if (_arg_int(2, n) < 0)
            return -3;

        if (f == nullptr)
            return -4;
        if (n <= 0)
            return -5;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        char *k_buf = new char[n + 1];
        int ret = f->read((uint64)k_buf, n, f->get_file_offset(), true);
        if (ret < 0)
            return -6;
        if (mem::k_vmm.copy_out(*pt, buf, k_buf, ret) < 0)
            return -7;

        delete[] k_buf;
        return ret;
    }
    uint64 SyscallHandler::sys_kill()
    {
        TODO("sys_kill");
        printfYellow("sys_kill\n");
        return 0;
    }
    uint64 SyscallHandler::sys_execve()
    {
        uint64 uargv, uenvp;

        eastl::string path;
        if (_arg_str(0, path, PGSIZE) < 0 ||
            _arg_addr(1, uargv) < 0 || _arg_addr(2, uenvp) < 0)
            return -1;
        printfCyan("execve fetch path=%s\n", path);
        printfCyan("execve fetch argv=%p\n", uargv);
        printfCyan("execve fetch envp=%p\n", uenvp);

        TODO("sys_execve");
        return 0;
    }
    uint64 SyscallHandler::sys_fstat()
    {
        int fd;
        fs::Kstat kst;
        uint64 kst_addr;
        if (_arg_fd(0, &fd, nullptr) < 0 || _arg_addr(1, kst_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_fstat] Error fetching arguments\n");
            return -1;
        }
        proc::k_pm.fstat(fd, &kst);
        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        // 检查 kst_addr 是否在用户空间
        if (mem::k_vmm.copy_out(*pt, kst_addr, &kst, sizeof(kst)) < 0)
        {
            printfRed("[SyscallHandler::sys_fstat] Error copying out kstat\n");
            return -1;
        }
        // 返回成功
        return 0;
    }
    uint64 SyscallHandler::sys_statx()
    {
        using __u16 = uint16;
        using __u32 = uint32;
        using __s64 = int64;
        using __u64 = uint64;

        struct statx_timestamp
        {
            __s64 tv_sec;  /* Seconds since the Epoch (UNIX time) */
            __u32 tv_nsec; /* Nanoseconds since tv_sec */
        };
        struct statx
        {
            __u32 stx_mask;       /* Mask of bits indicating
                                     filled fields */
            __u32 stx_blksize;    /* Block size for filesystem I/O */
            __u64 stx_attributes; /* Extra file attribute indicators */
            __u32 stx_nlink;      /* Number of hard links */
            __u32 stx_uid;        /* User ID of owner */
            __u32 stx_gid;        /* Group ID of owner */
            __u16 stx_mode;       /* File type and mode */
            __u64 stx_ino;        /* Inode number */
            __u64 stx_size;       /* Total size in bytes */
            __u64 stx_blocks;     /* Number of 512B blocks allocated */
            __u64 stx_attributes_mask;
            /* Mask to show what's supported
               in stx_attributes */

            /* The following fields are file timestamps */
            struct statx_timestamp stx_atime; /* Last access */
            struct statx_timestamp stx_btime; /* Creation */
            struct statx_timestamp stx_ctime; /* Last status change */
            struct statx_timestamp stx_mtime; /* Last modification */

            /* If this file represents a device, then the next two
               fields contain the ID of the device */
            __u32 stx_rdev_major; /* Major ID */
            __u32 stx_rdev_minor; /* Minor ID */

            /* The next two fields contain the ID of the device
               containing the filesystem where the file resides */
            __u32 stx_dev_major; /* Major ID */
            __u32 stx_dev_minor; /* Minor ID */

            __u64 stx_mnt_id; /* Mount ID */

            /* Direct I/O alignment restrictions */
            __u32 stx_dio_mem_align;
            __u32 stx_dio_offset_align;
        };

        int fd;
        eastl::string path_name;
        fs::Kstat kst;
        statx stx;
        uint64 kst_addr;

        if (_arg_int(0, fd) < 0)
            return -1;

        if (_arg_str(1, path_name, 128) < 0)
            return -1;

        if (_arg_addr(4, kst_addr) < 0)
            return -1;
        // 情况一：fd > 0，说明调用者提供了一个已打开的文件描述符
        if (fd > 0)
        {
            proc::k_pm.fstat(fd, &kst);
            stx.stx_mode = kst.mode;
            stx.stx_size = kst.size;
            stx.stx_dev_minor = (kst.dev) >> 32;
            stx.stx_dev_major = (kst.dev) & (0xFFFFFFFF);
            // dev=(int)(stx_dev_major<<8 | stx_dev_minor&0xff)
            stx.stx_ino = kst.ino;
            stx.stx_nlink = kst.nlink;
            stx.stx_atime.tv_sec = kst.st_atime_sec;
            stx.stx_atime.tv_nsec = kst.st_atime_nsec;
            stx.stx_mtime.tv_sec = kst.st_mtime_sec;
            stx.stx_mtime.tv_nsec = kst.st_mtime_nsec;
            stx.stx_ctime.tv_sec = kst.st_ctime_sec;
            stx.stx_ctime.tv_nsec = kst.st_ctime_nsec;
            // 将 statx 结构体拷贝回用户地址空间
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
            if (mem::k_vmm.copy_out(*pt, kst_addr, &stx, sizeof(stx)) < 0)
                return -1;
            return 0;
        }
        else
        {
            // 情况二：fd <= 0，说明系统调用者提供的是路径名而非fd
            // 使用 open + fstat + close 组合临时获取元数据
            int ffd;
            ffd = proc::k_pm.open(fd, path_name, 2);
            if (ffd < 0)
                return -1;
            proc::k_pm.fstat(ffd, &kst);
            proc::k_pm.close(ffd);
            // 填充 statx（此处简化为只填 mode 和 size）
            stx.stx_size = kst.size;
            stx.stx_mode = kst.mode;
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
            if (mem::k_vmm.copy_out(*pt, kst_addr, &stx, sizeof(stx)) < 0)
                return -1;
            return 0;
        }
    }
    uint64 SyscallHandler::sys_chdir()
    {
        eastl::string path;
        if (_arg_str(0, path, path.max_size()) < 0)
        {
            printfRed("[SyscallHandler::sys_chdir] Error fetching path argument\n");
            return -1;
        }
        printfCyan("[SyscallHandler::sys_chdir] Changing directory to: %s\n", path.c_str());
        // 调用进程管理器的 chdir 函数
        return proc::k_pm.chdir(path);
    }
    uint64 SyscallHandler::sys_dup()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f;
        int fd;
        [[maybe_unused]] int oldfd = 0;

        if (_arg_fd(0, &oldfd, &f) < 0 || (fd = proc::k_pm.alloc_fd(p, f)) < 0)
        {
            printfRed("[SyscallHandler::sys_dup] Error fetching arguments or allocating fd\n");
            return -1;
        }
        // fs::k_file_table.dup( f );
        f->dup();
        return fd;
    }
    uint64 SyscallHandler::sys_sleep()
    {
        tmm::timeval tv;
        uint64 tv_addr;
        if (_arg_addr(0, tv_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_sleep] Error fetching timeval address\n");
            return -1;
        }
        if (mem::k_vmm.copy_in(*proc::k_pm.get_cur_pcb()->get_pagetable(), &tv, tv_addr, sizeof(tv)) < 0)
        {
            printfRed("[SyscallHandler::sys_sleep] Error copying timeval from userspace\n");
            return -1;
        }
        return tmm::k_tm.sleep_from_tv(tv);
    }
    uint64 SyscallHandler::sys_uptime()
    {
        TODO("sys_uptime");
        printfYellow("sys_uptime\n");
        return 0;
    }
    uint64 SyscallHandler::sys_openat()
    {
        int dir_fd;
        uint64 path_addr;
        int flags;

        if (_arg_int(0, dir_fd) < 0)
            return -1;
        if (_arg_addr(1, path_addr) < 0)
            return -1;
        if (_arg_int(2, flags) < 0)
            return -1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        eastl::string path;
        if (mem::k_vmm.copy_str_in(*pt, path, path_addr, 100) < 0)
            return -1;
        int res = proc::k_pm.open(dir_fd, path, flags);
        printfBlue("openat return fd is %d", res);
        return res;
    }
    uint64 SyscallHandler::sys_write()
    {

        fs::file *f;
        int n;
        uint64 p;
        [[maybe_unused]] int fd = 0;
        if (_arg_fd(0, &fd, &f) < 0 || _arg_addr(1, p) < 0 ||
            _arg_int(2, n) < 0)
        {
            panic("[SyscallHandler::sys_write] Error fetching arguments");
            return -1;
        }
        if (fd > 2)
            printfRed("invoke sys_write\n");
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = proc->get_pagetable();
        char *buf = new char[n + 10];
        {
            mem::UserspaceStream uspace((void *)p, n + 1, pt);
            uspace.open();
            mem::UsRangeDesc urd = std::make_tuple((u8 *)buf, (ulong)n + 1);
            uspace >> urd;
            uspace.close();
        }
        long rc = f->write((ulong)buf, n, f->get_file_offset(), true);
        delete[] buf;
        return rc;
    }

    uint64 SyscallHandler::sys_unlinkat()
    {
        TODO("sys_unlinkat");
        printfYellow("sys_unlinkat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_linkat()
    {
        TODO("sys_linkat");
        printfYellow("sys_linkat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mkdirat()
    {
        TODO("sys_mkdirat");
        printfYellow("sys_mkdirat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_close()
    {
        int fd;
        if (_arg_int(0, fd) < 0)
            return -1;
        return proc::k_pm.close(fd);
    }
    uint64 SyscallHandler::sys_mknod()
    {
        TODO("sys_mkn");
        printfYellow("sys_mkn\n");
        return 0;
    }
    uint64 SyscallHandler::sys_clone()
    {
        TODO("TBF")
        int flags;
        uint64 stack, tls, ctid, ptid;
        _arg_int(0, flags);
        _arg_addr(1, stack);
        _arg_addr(2, ptid);
        _arg_addr(3, tls);
        _arg_addr(4, ctid);
        if(flags != SIGCHILD){
            panic("[SyscallHandler::sys_clone] flags must be SIGCHILD");
        }
        return proc::k_pm.fork();
    }
    uint64 SyscallHandler::sys_umount2()
    {
        TODO("sys_umount2");
        printfYellow("sys_umount2\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mount()
    {
        uint64 dev_addr;
        uint64 mnt_addr;
        uint64 fstype_addr;
        eastl::string dev;
        eastl::string mnt;
        eastl::string fstype;
        int flags;
        uint64 data;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        if (_arg_addr(0, dev_addr) < 0)
            return -1;
        if (_arg_addr(1, mnt_addr) < 0)
            return -1;
        if (_arg_addr(2, fstype_addr) < 0)
            return -1;

        if (mem::k_vmm.copy_str_in(*pt, dev, dev_addr, 100) < 0)
            return -1;
        if (mem::k_vmm.copy_str_in(*pt, mnt, mnt_addr, 100) < 0)
            return -1;
        if (mem::k_vmm.copy_str_in(*pt, fstype, fstype_addr, 100) < 0)
            return -1;

        if (_arg_int(3, flags) < 0)
            return -1;
        if (_arg_addr(4, data) < 0)
            return -1;

        // return proc::k_pm.mount( dev, mnt, fstype, flags, data );
        fs::Path devpath(dev);
        fs::Path mntpath(mnt);

        return mntpath.mount(devpath, fstype, flags, data);
    }
    uint64 SyscallHandler::sys_brk()
    {
        uint64 n;
        // 此处是内存扩展到n地址
        if (_arg_addr(0, n) < 0)
        {
            printfRed("[SyscallHandler::sys_brk] Error fetching brk address\n");
            return -1;
        }
        return proc::k_pm.brk(n); // 调用进程管理器的 brk 函数
    }
    uint64 SyscallHandler::sys_munmap()
    {
        u64 start;
        size_t size;
        if (_arg_addr(0, start) < 0 || _arg_addr(1, size) < 0)
        {
            printfRed("[SyscallHandler::sys_munmap] Error fetching munmap arguments\n");
            return -1;
        }
        return proc::k_pm.munmap((void *)start, size); // 调用进程管理器的 munmap 函数
    }

    uint64 SyscallHandler::sys_mmap()
    {
        u64 addr;
        size_t map_size;
        int prot;
        int flags;
        int fd;
        size_t offset;
        if (_arg_addr(0, addr) < 0 || _arg_addr(1, map_size) < 0 || _arg_int(2, prot) < 0 ||
            _arg_int(3, flags) < 0 || _arg_int(4, fd) < 0 || _arg_addr(5, offset) < 0)
        {
            printfRed("[SyscallHandler::sys_mmap] Error fetching mmap arguments\n");
            return -1;
        }
        return (uint64)proc::k_pm.mmap((void *)addr, map_size, prot, flags, fd, offset); // 调用进程管理器的 mmap 函数
    }

    uint64 SyscallHandler::sys_times()
    {
        TODO("sys_times");
        printfYellow("sys_times\n");
        return 0;
    }
    uint64 SyscallHandler::sys_uname()
    {
        TODO("sys_uname");
        printfYellow("sys_uname\n");
        return 0;
    }
    uint64 SyscallHandler::sys_sched_yield()
    {
        TODO("sys_sched_yield");
        printfYellow("sys_sched_yield\n");
        return 0;
    }
    uint64 SyscallHandler::sys_gettimeofday()
    {
        TODO("sys_gettimeofday");
        printfYellow("sys_gettimeofday\n");
        return 0;
    }
    uint64 SyscallHandler::sys_nanosleep()
    {
        TODO("sys_nanosleep");
        printfYellow("sys_nanosleep\n");
        return 0;
    }
    uint64 SyscallHandler::sys_getcwd()
    {
        char cwd[256];
        uint64 buf;
        int size;

        if (_arg_addr(0, buf) < 0)
            return -1;
        if (_arg_int(1, size) < 0)
            return -1;
        if (size >= (int)sizeof(cwd))
            return -1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        uint len = proc::k_pm.getcwd(cwd);
        if (mem::k_vmm.copy_out(*pt, buf, (const void *)cwd, len) < 0)
            return -1;

        return buf;
    }
    uint64 SyscallHandler::sys_getdents64()
    {
        TODO("sys_getdents64");
        printfYellow("sys_getdents64\n");
        return 0;
    }
    uint64 SyscallHandler::sys_shutdown()
    {
        TODO(struct filesystem *fs = get_fs_from_path("/");
             vfs_ext_umount(fs);)
        sbi_shutdown();
        printfYellow("sys_shutdown\n");
        sbi_shutdown();
        return 0;
    }

    //====================================signal===================================================
    uint64 sys_kill_signal()
    {
        ///@todo
        return 0;
    }
    uint64 sys_tkill()
    {
        ///@todo
        return 0;
    }
    uint64 sys_tgkill()
    {
        ///@todo
        return 0;
    }
    uint64 sys_rt_sigaction()
    {
        ///@todo
        return 0;
    }
    uint64 sys_rt_sigprocmask()
    {
        ///@todo
        return 0;
    }
    uint64 sys_rt_sigtimedwait()
    {
        ///@todo
        return 0;
    }
    uint64 sys_rt_sigreturn()
    {
        ///@todo
        return 0;
    }

    //================================== busybox===================================================
    uint64 SyscallHandler::sys_set_tid_address()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_getuid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_getgid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_setgid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_setuid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_fstatat()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::SyscallHandler::sys_exit_group()
    {
        int status;
        if (_arg_int(0, status) < 0)
        {
            printfRed("[SyscallHandler::sys_exit_group] Error fetching exit status\n");
            return -1;
        }
        proc::k_pm.exit_group(status); // 调用进程管理器的 exit_group 函数
        return -1;                     // 退出后不应该返回
    }

    uint64 SyscallHandler::sys_set_robust_list()
    {
        ulong addr;
        proc::robust_list_head *head;
        size_t len;
        if (_arg_addr(0, addr) < 0 || _arg_addr(1, len) < 0)
        {
            printfRed("[SyscallHandler::sys_set_robust_list] Error fetching arguments\n");
            return -1;
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
#ifdef RISCV
        head = (proc::robust_list_head *)pt->walk_addr(addr);
#elif defined(LOONGARCH)
        head = (proc::robust_list_head *)hsai::k_mem->to_vir(
            pt->walk_addr(addr));
#endif
        if (head == nullptr)
            return -10;

        return proc::k_pm.set_robust_list(head, len); // 调用进程管理器的 set_robust_list 函数
    }
    uint64 SyscallHandler::sys_gettid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_writev()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::SyscallHandler::sys_prlimit64()
    {
        int pid;
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_prlimit64] Error fetching pid argument\n");
            return -1;
        }
        int rsrc;
        if (_arg_int(1, rsrc) < 0)
        {
            printfRed("[SyscallHandler::sys_prlimit64] Error fetching resource argument\n");
            return -2;
        }
        u64 new_limit;
        u64 old_limit;
        if (_arg_addr(2, new_limit) < 0)
        {
            printfRed("[SyscallHandler::sys_prlimit64] Error fetching new limit address\n");
            return -3;
        }
        if (_arg_addr(3, old_limit) < 0)
        {
            printfRed("[SyscallHandler::sys_prlimit64] Error fetching old limit address\n");
            return -4;
        }

        proc::rlimit64 *nlim = nullptr, *olim = nullptr;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
#ifdef RISCV
        if (new_limit != 0)
            nlim = (proc::rlimit64 *)pt->walk_addr(new_limit);
        if (old_limit != 0)
            olim = (proc::rlimit64 *)pt->walk_addr(old_limit);

#elif defined(LOONGARCH)
        if (new_limit != 0)
            nlim = (proc::rlimit64 *)hsai::k_mem->to_vir(
                pt->walk_addr(new_limit));
        if (old_limit != 0)
            olim = (proc::rlimit64 *)hsai::k_mem->to_vir(
                pt->walk_addr(old_limit));
#endif

        return proc::k_pm.prlimit64(pid, rsrc, nlim, olim);
        ;
    }
    uint64 SyscallHandler::sys_readlinkat()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int fd;
        fs::Path filePath;
        size_t ret;

        if (_arg_int(0, fd) < 0)
            return -1;

        eastl::string path;
        if (_arg_str(1, path, 256) < 0)
            return -1;

        uint64 buf;
        if (_arg_addr(2, buf) < 0)
            return -1;

        size_t buf_size;
        if (_arg_addr(3, buf_size) < 0)
            return -1;

        if (fd == AT_FDCWD)
            new (&filePath) fs::Path(path, p->_cwd);
        else
            new (&filePath) fs::Path(path, p->_ofile[fd]);

        fs::dentry *dent = filePath.pathSearch();
        if (dent == nullptr)
            return -1;

        char *buffer = new char[buf_size];
        ret = dent->getNode()->readlinkat(buffer, buf_size);

        if (mem::k_vmm.copy_out(*pt, buf, (void *)buffer, ret) < 0)
        {
            delete[] buffer;
            return -1;
        }

        delete[] buffer;
        return ret;
    }
    uint64 SyscallHandler::sys_getrandom()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_clock_gettime()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_ioctl()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_syslog()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_fcntl()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_faceessat()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_sysinfo()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_ppoll()
    {
        // TODO
        return 0;
    }

    uint64 SyscallHandler::sys_sendfile()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_readv()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_geteuid()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_madvise()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_mremap()
    {
        // TODO
        return 0;
    }

    uint64 SyscallHandler::sys_lseek()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_utimensat()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_renameat2()
    {
        // TODO
        return 0;
    }

    uint64 SyscallHandler::sys_clock_nanosleep()
    {
        // TODO
        return 0;
    }
}

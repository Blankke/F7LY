#include "syscall_handler.hh"
#include "printer.hh"
#include "proc.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "userspace_stream.hh"
#include "klib.hh"
#include "list.hh"
#include "param.h"
#include "sbi.hh"
#include "hal/cpu.hh"
#include "timer_manager.hh"
#include "fs/vfs/path.hh"
#include "fs/vfs/file/device_file.hh"
#include <asm-generic/ioctls.h>
#include <asm-generic/poll.h>
#include <linux/sysinfo.h>
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "proc/pipe.hh"
#include "proc/signal.hh"
#include "scheduler.hh"
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
        BIND_SYSCALL(fork);
        BIND_SYSCALL(wait);
        BIND_SYSCALL(kill);
        BIND_SYSCALL(sleep);
        BIND_SYSCALL(uptime);
        BIND_SYSCALL(mknod);
        BIND_SYSCALL(getcwd);
        BIND_SYSCALL(shutdown);
        BIND_SYSCALL(dup);
        BIND_SYSCALL(dup3);
        BIND_SYSCALL(fcntl);
        BIND_SYSCALL(ioctl);
        BIND_SYSCALL(mkdirat);
        BIND_SYSCALL(unlinkat);
        BIND_SYSCALL(linkat);
        BIND_SYSCALL(umount2);
        BIND_SYSCALL(mount);
        BIND_SYSCALL(faceessat);
        BIND_SYSCALL(chdir);
        BIND_SYSCALL(exec);
        BIND_SYSCALL(openat);
        BIND_SYSCALL(close);
        BIND_SYSCALL(pipe2);
        BIND_SYSCALL(getdents64);
        BIND_SYSCALL(lseek);
        BIND_SYSCALL(read);
        BIND_SYSCALL(write);
        BIND_SYSCALL(readv);
        BIND_SYSCALL(writev);
        BIND_SYSCALL(sendfile);
        BIND_SYSCALL(ppoll);
        BIND_SYSCALL(readlinkat);
        BIND_SYSCALL(fstatat);
        BIND_SYSCALL(fstat);
        BIND_SYSCALL(utimensat);
        BIND_SYSCALL(exit);
        BIND_SYSCALL(exit_group);
        BIND_SYSCALL(set_tid_address);
        BIND_SYSCALL(set_robust_list);
        BIND_SYSCALL(nanosleep);
        BIND_SYSCALL(clock_gettime);
        BIND_SYSCALL(clock_nanosleep);
        BIND_SYSCALL(syslog);
        BIND_SYSCALL(sched_yield);
        BIND_SYSCALL(kill_signal);
        BIND_SYSCALL(tkill);
        BIND_SYSCALL(tgkill);
        BIND_SYSCALL(rt_sigaction);
        BIND_SYSCALL(rt_sigprocmask);
        BIND_SYSCALL(rt_sigtimedwait);
        BIND_SYSCALL(rt_sigreturn);
        BIND_SYSCALL(setgid);
        BIND_SYSCALL(setuid);
        BIND_SYSCALL(times);
        BIND_SYSCALL(uname);
        BIND_SYSCALL(gettimeofday);
        BIND_SYSCALL(getpid);
        BIND_SYSCALL(getppid);
        BIND_SYSCALL(getuid);
        BIND_SYSCALL(geteuid);
        BIND_SYSCALL(getgid);
        BIND_SYSCALL(gettid);
        BIND_SYSCALL(sysinfo);
        BIND_SYSCALL(brk);
        BIND_SYSCALL(munmap);
        BIND_SYSCALL(mremap);
        BIND_SYSCALL(clone);
        BIND_SYSCALL(execve);
        BIND_SYSCALL(mmap);
        BIND_SYSCALL(madvise);
        BIND_SYSCALL(wait4);
        BIND_SYSCALL(prlimit64);
        BIND_SYSCALL(renameat2);
        BIND_SYSCALL(getrandom);
        BIND_SYSCALL(statx);
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
        // printf("[SyscallHandler::invoke_syscaller]invoke syscall handler\n");
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        uint64 sys_num = p->get_trapframe()->a7; // 获取系统调用号
        // if (sys_num != 64)
            // printfGreen("[invoke_syscaller]sys_num: %d sys_name: %s\n", sys_num, _syscall_name[sys_num]);
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
        int waitret = proc::k_pm.wait4(pid, wstatus_addr, 0);
        printf("[SyscallHandler::sys_wait] waitret: %d",
               waitret);
        return waitret;
    }
    uint64 SyscallHandler::sys_wait4()
    {
        int pid;
        uint64 wstatus_addr;
        int option;
        if (_arg_int(0, pid) < 0)
            return -1;
        if (_arg_addr(1, wstatus_addr) < 0)
            return -1;
        if (_arg_int(2, option) < 0)
            return -1;
        return proc::k_pm.wait4(pid, wstatus_addr, 0);
    }
    uint64 SyscallHandler::sys_getppid()
    {
        return proc::k_pm.get_cur_pcb()->get_ppid();
    }
    uint64 SyscallHandler::sys_getpid()
    {
        return proc::k_pm.get_cur_pcb()->get_pid();
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
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f;
        int fd;
        [[maybe_unused]] int oldfd = 0;

        if (_arg_fd(0, &oldfd, &f) < 0)
            return -1;
        if (_arg_int(1, fd) < 0)
            return -1;
        if (fd == oldfd)
            return fd;
        if (proc::k_pm.alloc_fd(p, f, fd) < 0)
            return -1;
        // fs::k_file_table.dup( f );
        f->dup();
        return fd;
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
        // printfCyan("[sys_read] Try read,f:%x,buf:%x", f, f);
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
        int pid;
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_wait] Error fetching arguments\n");
            return -1;
        }
        return proc::k_pm.kill_proc(pid);
    }
    uint64 SyscallHandler::sys_execve()
    {
        uint64 uargv, uenvp;

        eastl::string path;
        if (_arg_str(0, path, PGSIZE) < 0 ||
            _arg_addr(1, uargv) < 0 || _arg_addr(2, uenvp) < 0)
            return -1;

        eastl::vector<eastl::string> argv;
        uint64 uarg;
        if (uargv != 0)
        {
            for (uint64 i = 0, puarg = uargv;; i++, puarg += sizeof(char *))
            {
                if (i >= max_arg_num)
                    return -1;

                if (_fetch_addr(puarg, uarg) < 0)
                    return -1;

                if (uarg == 0)
                    break;

                // printfCyan( "execve get arga[%d] = %p\n", i, uarg );

                argv.emplace_back(eastl::string());
                if (_fetch_str(uarg, argv[i], PGSIZE) < 0)
                    return -1;
                // printfCyan("execve get arga[%d] = %s\n", i, argv[i].c_str());
            }
        }

        eastl::vector<eastl::string> envp;
        ulong uenv;
        if (uenvp != 0)
        {
            for (ulong i = 0, puenv = uenvp;; i++, puenv += sizeof(char *))
            {
                if (i >= max_arg_num)
                    return -2;

                if (_fetch_addr(puenv, uenv) < 0)
                    return -2;

                if (uenv == 0)
                    break;

                envp.emplace_back(eastl::string());
                if (_fetch_str(uenv, envp[i], PGSIZE) < 0)
                    return -2;
                // printfCyan("execve get envp[%d] = %s\n", i, envp[i].c_str());
            }
        }

        return proc::k_pm.execve(path, argv, envp);
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
        int n;
        if (_arg_int(0, n) < 0)
        {
            printfRed("[SyscallHandler::sys_sleep] Error fetching n argument\n");
            return -1;
        }
        n /= 2;
        return tmm::k_tm.sleep_n_ticks(n);
    }
    uint64 SyscallHandler::sys_uptime()
    {
        return tmm::k_tm.get_ticks();
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
        // printfBlue("openat return fd is %d", res);
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
        // if (fd > 2)
        //     printfRed("invoke sys_write\n");
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
        // copy from Li
        int fd, flags;
        uint64 path_addr;

        if (_arg_int(0, fd) < 0)
            return -1;
        if (_arg_addr(1, path_addr) < 0)
            return -1;
        if (_arg_int(2, flags) < 0)
            return -1;
        eastl::string path;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_str_in(*pt, path, path_addr, 100) < 0)
            return -1;

        int res = proc::k_pm.unlink(fd, path, flags);
        return res;
    }
    uint64 SyscallHandler::sys_linkat()
    {
        TODO("sys_linkat");
        printfYellow("sys_linkat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mkdirat()
    {
        // // 抄的学长的sys_mkdir
        // printfBlue("mkdir!\n");
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

        int res = proc::k_pm.mkdir(dir_fd, path, flags);
        return res;
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
        // printfYellow("sys_clone\n");
        int flags;
        uint64 stack, tls, ctid, ptid;
        _arg_int(0, flags);
        _arg_addr(1, stack);
        _arg_addr(2, ptid);
        _arg_addr(3, tls);
        _arg_addr(4, ctid);
        if (flags != SIGCHILD)
        {
            panic("[SyscallHandler::sys_clone] flags must be SIGCHILD");
        }
        return proc::k_pm.fork(stack);
    }
    uint64 SyscallHandler::sys_umount2()
    {
        uint64 specialaddr;
        eastl::string special;
        int flags;

        proc::Pcb *cur = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur->get_pagetable();

        if (_arg_addr(0, specialaddr) < 0)
            return -1;
        if (_arg_int(1, flags) < 0)
            return -1;

        if (mem::k_vmm.copy_str_in(*pt, special, specialaddr, 100) < 0)
            return -1;

        fs::Path specialpath(special);
        return specialpath.umount(flags);
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
        int size;
        if (_arg_addr(0, start) < 0 || _arg_int(1, size) < 0)
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
        // TODO: 检查一下有没有错
        tmm::tms tms_val;
        uint64 tms_addr;

        if (_arg_addr(0, tms_addr) < 0)
            return -1;

        proc::k_pm.get_cur_proc_tms(&tms_val);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, tms_addr, &tms_val, sizeof(tms_val)) <
            0)
            return -1;

        return tmm::k_tm.get_ticks();
    }
    struct _Utsname
    {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    };
    static const char _SYSINFO_sysname[] = "F7LY-OS";
    static const char _SYSINFO_nodename[] = "(none-node)";
    static const char _SYSINFO_release[] = "4.15.0";
    static const char _SYSINFO_version[] = "4.15.0";
    static const char _SYSINFO_machine[] = "Riscv";
    static const char _SYSINFO_domainname[] = "(none-domain)";
    uint64 SyscallHandler::sys_uname()
    {
        uint64 usta;
        uint64 sysa, noda, rlsa, vsna, mcha, dmna;

        if (_arg_addr(0, usta) < 0)
            return -11;
        sysa = (uint64)(((_Utsname *)usta)->sysname);
        noda = (uint64)(((_Utsname *)usta)->nodename);
        rlsa = (uint64)(((_Utsname *)usta)->release);
        vsna = (uint64)(((_Utsname *)usta)->version);
        mcha = (uint64)(((_Utsname *)usta)->machine);
        dmna = (uint64)(((_Utsname *)usta)->domainname);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        if (mem::k_vmm.copy_out(*pt, sysa, _SYSINFO_sysname,
                                sizeof(_SYSINFO_sysname)) < 0)
            return -1;
        if (mem::k_vmm.copy_out(*pt, noda, _SYSINFO_nodename,
                                sizeof(_SYSINFO_nodename)) < 0)
            return -1;
        if (mem::k_vmm.copy_out(*pt, rlsa, _SYSINFO_release,
                                sizeof(_SYSINFO_release)) < 0)
            return -1;
        if (mem::k_vmm.copy_out(*pt, vsna, _SYSINFO_version,
                                sizeof(_SYSINFO_version)) < 0)
            return -1;
        if (mem::k_vmm.copy_out(*pt, mcha, _SYSINFO_machine,
                                sizeof(_SYSINFO_machine)) < 0)
            return -1;
        if (mem::k_vmm.copy_out(*pt, dmna, _SYSINFO_domainname,
                                sizeof(_SYSINFO_domainname)) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_sched_yield()
    {
        proc::k_scheduler.yield();
        return 0;
    }
    uint64 SyscallHandler::sys_gettimeofday()
    {
        // TODO: 检查一下这个
        uint64 tv_addr;
        tmm::timeval tv;

        if (_arg_addr(0, tv_addr) < 0)
            return -1;

        tv = tmm::k_tm.get_time_val();

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, tv_addr, (const void *)&tv,
                                sizeof(tv)) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_nanosleep()
    {

        int clockid;
        int flags;
        timespec dur;
        uint64 dur_addr;
        timespec rem;
        uint64 rem_addr;
        if (_arg_int(0, clockid) < 0 || _arg_int(1, flags) < 0 ||
            _arg_addr(0, dur_addr) < 0 || _arg_addr(1, rem_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_nanosleep] Error fetching nanosleep arguments\n");
            return -1;
        }

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur_proc->get_pagetable();

        if (dur_addr != 0)
            if (mem::k_vmm.copy_in(*pt, &dur, dur_addr, sizeof(dur)) < 0)
                return -1;
        // printfCyan("into nano sleep,dur_addr:%p.rem_addr:%p\n", dur_addr, rem_addr);
        if (rem_addr != 0)
            if (mem::k_vmm.copy_in(*pt, &rem, rem_addr, sizeof(rem)) < 0)
                return -1;

        tmm::timeval tm_;
        tm_.tv_sec = dur.tv_sec;
        tm_.tv_usec = dur.tv_nsec / 1000;

        tmm::k_tm.sleep_from_tv(tm_);

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
        fs::file *f;
        uint64 buf_addr;
        uint64 buf_len;
        if (_arg_fd(0, nullptr, &f) < 0)
            return -1;
        if (_arg_addr(1, buf_addr) < 0)
            return -1;
        if (_arg_addr(2, buf_len) < 0)
            return -1;

        if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL &&
            f->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            return -1;
        // eastl::string name = f->data.get_Entry()->rName();
        fs::normal_file *normal_f = static_cast<fs::normal_file *>(f);

        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();

        mem::UserspaceStream us((void *)buf_addr, buf_len, pt);

        us.open();
        u64 rlen = us.rest_space();
        normal_f->read_sub_dir(us);
        rlen -= us.rest_space();
        us.close();

        return rlen;
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
    uint64 SyscallHandler::sys_kill_signal()
    {
        ///@todo
        return 0;
    }
    uint64 SyscallHandler::sys_tkill()
    {
        ///@todo
        return 0;
    }
    uint64 SyscallHandler::sys_tgkill()
    {
        ///@todo
        return 0;
    }
    uint64 SyscallHandler::sys_rt_sigaction()
    {
        return 0;
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        [[maybe_unused]] mem::PageTable *pt = proc->get_pagetable();
        [[maybe_unused]] proc::ipc::signal::sigaction a_newact, a_oldact;
        // a_newact = nullptr;
        // a_oldact = nullptr;
        uint64 newactaddr, oldactaddr;
        int flag;
        int ret = -1;

        if (_arg_int(0, flag) < 0)
            return -1;

        if (_arg_addr(1, newactaddr) < 0)
            return -1;

        if (_arg_addr(2, oldactaddr) < 0)
            return -1;

        if (oldactaddr != 0)
            a_oldact = proc::ipc::signal::sigaction();

        if (newactaddr != 0)
        {
            if (mem::k_vmm.copy_in(*pt, &a_newact, newactaddr,
                                   sizeof(proc::ipc::signal::sigaction)) < 0)
                return -1;
            // a_newact = ( pm::ipc::signal::sigaction *)(hsai::k_mem->to_vir(
            // pt->walk_addr( newactaddr ) ));
            ret = proc::ipc::signal::sigAction(flag, &a_newact, nullptr);
        }
        else
        {
            ret = proc::ipc::signal::sigAction(flag, &a_newact, &a_oldact);
        }
        if (ret == 0 && oldactaddr != 0)
        {
            if (mem::k_vmm.copy_out(*pt, oldactaddr, &a_oldact,
                                    sizeof(proc::ipc::signal::sigaction)) < 0)
                return -1;
        }
        return ret;
    }
    uint64 SyscallHandler::sys_rt_sigprocmask()
    {
        return 0;
        int how;
        signal::sigset_t set;
        signal::sigset_t old_set;
        uint64 setaddr;
        uint64 oldsetaddr;
        int sigsize;

        if (_arg_int(0, how) < 0)
            return -1;
        if (_arg_addr(1, setaddr) < 0)
            return -1;
        if (_arg_addr(2, oldsetaddr) < 0)
            return -1;
        if (_arg_int(3, sigsize) < 0)
            return -1;

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur_proc->get_pagetable();

        if (setaddr != 0)
            if (mem::k_vmm.copy_in(*pt, &set, setaddr, sizeof(signal::sigset_t)) < 0)
                return -1;
        if (oldsetaddr != 0)
            if (mem::k_vmm.copy_in(*pt, &old_set, oldsetaddr, sizeof(signal::sigset_t)) < 0)
                return -1;

        return signal::sigprocmask(how, &set, &old_set, sigsize);
    }
    uint64 SyscallHandler::sys_rt_sigtimedwait()
    {
        ///@todo
        return 0;
    }
    uint64 SyscallHandler::sys_rt_sigreturn()
    {
        ///@todo
        return 0;
    }

    //================================== busybox===================================================
    uint64 SyscallHandler::sys_set_tid_address()
    {
        uint64 tidptr;
        if (_arg_addr(0, tidptr) < 0)
        {
            printfRed("[SyscallHandler::sys_set_tid_address] Error fetching tidptr argument\n");
            return -1;
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        p->clear_child_tid = tidptr;
        return p->get_pid();
    }
    uint64 SyscallHandler::sys_getuid()
    {
        // TODO//我们root用户id就是1
        return 1;
    }
    uint64 SyscallHandler::sys_getgid()
    {
        // TODO
        return 1; // 直接返回1，抄学长的
    }
    uint64 SyscallHandler::sys_setgid()
    {
        // TODO
        return 1; // 直接返回1，抄学长的
    }
    uint64 SyscallHandler::sys_setuid()
    {
        // TODO
        return 1; // 直接返回1，抄学长的
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
        return proc::k_pm.get_cur_pcb()->get_pid();
    }
    uint64 SyscallHandler::sys_writev()
    {
        fs::file *f;
        int fd = 0;
        int iovcnt;
        uint64 iov_ptr;

        // 获取参数
        if (_arg_fd(0, &fd, &f) < 0 ||
            _arg_addr(1, iov_ptr) < 0 ||
            _arg_int(2, iovcnt) < 0)
        {
            panic("[SyscallHandler::sys_writev] Error fetching arguments");
            return -1;
        }

        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = proc->get_pagetable();

        uint64 writebytes = 0;

        for (int i = 0; i < iovcnt; i++)
        {
            struct iovec iov;
            uint64 iov_addr = iov_ptr + i * sizeof(struct iovec);

            {
                mem::UserspaceStream uspace((void *)iov_addr, sizeof(struct iovec), pt);
                uspace.open();
                mem::UsRangeDesc urd = std::make_tuple((u8 *)&iov, sizeof(struct iovec));
                uspace >> urd;
                uspace.close();
            }

            char *buf = new char[iov.iov_len + 10];
            {
                mem::UserspaceStream uspace((void *)iov.iov_base, iov.iov_len + 1, pt);
                uspace.open();
                mem::UsRangeDesc urd = std::make_tuple((u8 *)buf, (ulong)iov.iov_len + 1);
                uspace >> urd;
                uspace.close();
            }

            long rc = f->write((ulong)buf, iov.iov_len, f->get_file_offset(), true);
            delete[] buf;

            if (rc < 0)
            {
                printfRed("[SyscallHandler::sys_writev] 写入文件失败\n");
                return -1;
            }

            writebytes += rc;
        }

        return writebytes;
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
        uint64 bufaddr;
        int buflen;
        [[maybe_unused]] int flags;
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = pcb->get_pagetable();

        if (_arg_addr(0, bufaddr) < 0)
            return -1;

        if (_arg_int(1, buflen) < 0)
            return -1;

        if (_arg_int(2, buflen) < 0)
            return -1;

        if (bufaddr == 0 && buflen == 0)
            return -1;

        char *k_buf = new char[buflen];
        if (!k_buf)
            return -1;

        ulong random = 0x4249'4C47'4B43'5546UL;
        size_t random_size = sizeof(random);
        for (size_t i = 0; i < static_cast<size_t>(buflen);
             i += random_size)
        {
            size_t copy_size =
                (i + random_size) <= static_cast<size_t>(buflen)
                    ? random_size
                    : buflen - i;
            memcpy(k_buf + i, &random, copy_size);
        }
        if (mem::k_vmm.copy_out(*pt, bufaddr, k_buf, buflen) < 0)
            return -1;

        delete[] k_buf;
        return buflen;
    }
    uint64 SyscallHandler::sys_clock_gettime()
    {
        int clock_id;
        u64 addr;
        if (_arg_int(0, clock_id) < 0)
        {
            printfRed("[SyscallHandler::sys_clock_gettime] Error fetching clock_id argument\n");
            return -1;
        }
        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_clock_gettime] Error fetching addr argument\n");
            return -2;
        }

        tmm::timespec *tp = nullptr;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (addr != 0)
#ifdef RISCV
            tp = (tmm::timespec *)pt->walk_addr(addr);
#elif LOONGARCH
            tp = (tmm::timespec *)hsai::k_mem->to_vir(pt->walk_addr(addr));
#endif
        tmm::SystemClockId cid = (tmm::SystemClockId)clock_id;

        return tmm::k_tm.clock_gettime(cid, tp);
        return 0;
    }
    uint64 SyscallHandler::sys_ioctl()
    {
        int tmp;

        fs::file *f = nullptr;
        int fd;
        if (_arg_fd(0, &fd, &f) < 0)
            return -1;
        if (f == nullptr)
            return -1;
        fd = fd;

        if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            return -1;

        u32 cmd;
        if (_arg_int(1, tmp) < 0)
            return -2;
        cmd = (u32)tmp;
        cmd = cmd;

        ulong arg;
        if (_arg_addr(2, arg) < 0)
            return -3;
        arg = arg;

        /// @todo not implement

        if ((cmd & 0xFFFF) == TCGETS)
        {
            fs::device_file *df = (fs::device_file *)f;
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            termios *ts = (termios *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            termios *ts =
                (termios *)hsai::k_mem->to_vir(pt->walk_addr(arg));
#endif
            return df->tcgetattr(ts);
        }

        if ((cmd & 0XFFFF) == TIOCGPGRP)
        {
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            int *p_pgrp = (int *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            int *p_pgrp = (int *)hsai::k_mem->to_vir(pt->walk_addr(arg));
#endif
            *p_pgrp = 1;
            return 0;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_syslog()
    {
        enum sys_log_type
        {

            SYSLOG_ACTION_CLOSE = 0,
            SYSLOG_ACTION_OPEN = 1,
            SYSLOG_ACTION_READ = 2,
            SYSLOG_ACTION_READ_ALL = 3,
            SYSLOG_ACTION_READ_CLEAR = 4,
            SYSLOG_ACTION_CLEAR = 5,
            SYSLOG_ACTION_CONSOLE_OFF = 6,
            SYSLOG_ACTION_CONSOLE_ON = 7,
            SYSLOG_ACTION_CONSOLE_LEVEL = 8,
            SYSLOG_ACTION_SIZE_UNREAD = 9,
            SYSLOG_ACTION_SIZE_BUFFER = 10

        };

        int prio;
        eastl::string fmt;
        uint64 fmt_addr;
        eastl::string msg = "Spectre V2 : Update user space SMT mitigation: STIBP always-on\n"
                            "process_manager : execve set stack-base = 0x0000_0000_9194_5000\n"
                            "proc/process_manager : execve set page containing sp is 0x0000_0000_9196_4000";
        [[maybe_unused]] proc::Pcb *p = proc::k_pm.get_cur_pcb();
        [[maybe_unused]] mem::PageTable *pt = p->get_pagetable();

        if (_arg_int(0, prio) < 0)
            return -1;

        if (_arg_addr(1, fmt_addr) < 0)
            return -1;

        if (prio == SYSLOG_ACTION_SIZE_BUFFER)
            return msg.size(); // 返回buffer的长度
        else if (prio == SYSLOG_ACTION_READ_ALL)
        {
            mem::k_vmm.copy_out(*pt, fmt_addr, msg.c_str(), msg.size());
            return msg.size();
        }

        return 0;
    }
    uint64 SyscallHandler::sys_fcntl()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = nullptr;
        int op;
        ulong arg;
        int retfd = -1;

        if (_arg_fd(0, nullptr, &f) < 0)
            return -1;
        if (_arg_int(1, op) < 0)
            return -2;

        switch (op)
        {
        case F_SETFD:
            if (_arg_addr(2, arg) < 0)
                return -3;
            if (arg & FD_CLOEXEC)
                f->_fl_cloexec = true;
            return 0;

        case F_DUPFD:
            if (_arg_addr(2, arg) < 0)
                return -3;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    f->refcnt++;
                    break;
                }
            }
            return retfd;

        case F_DUPFD_CLOEXEC:
            if (_arg_addr(2, arg) < 0)
                return -3;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    f->refcnt++;
                    break;
                }
            }
            p->get_open_file(retfd)->_fl_cloexec = true;
            return retfd;

        default:
            break;
        }

        return retfd;
    }
    uint64 SyscallHandler::sys_faceessat()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_sysinfo()
    {
        uint64 sysinfoaddr;
        [[maybe_unused]] sysinfo sysinfo_;

        if (_arg_addr(0, sysinfoaddr) < 0)
            return -1;

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur_proc->get_pagetable();

        memset(&sysinfo_, 0, sizeof(sysinfo_));
        sysinfo_.uptime = 0;
        sysinfo_.loads[0] = 0; // 负载均值  1min 5min 15min
        sysinfo_.loads[1] = 0;
        sysinfo_.loads[2] = 0;
        sysinfo_.totalram = 0; // 总内存
        sysinfo_.freeram = 0;
        sysinfo_.sharedram = 0;
        sysinfo_.bufferram = 0;
        sysinfo_.totalswap = 0;
        sysinfo_.freeswap = 0;
        sysinfo_.procs = 0;
        sysinfo_.pad = 0;
        sysinfo_.totalhigh = 0;
        sysinfo_.freehigh = 0;
        sysinfo_.mem_unit = 1; // 内存单位为 1 字节

        if (mem::k_vmm.copy_out(*pt, sysinfoaddr, &sysinfo_,
                                sizeof(sysinfo_)) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_ppoll()
    {
        uint64 fds_addr;
        uint64 timeout_addr;
        uint64 sigmask_addr;
        pollfd *fds = nullptr;
        int nfds;
        [[maybe_unused]] timespec tm{0, 0}; // 现在没用上
        [[maybe_unused]] sigset_t sigmask;  // 现在没用上
        [[maybe_unused]] int timeout;       // 现在没用上
        int ret = 0;

        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = proc->get_pagetable();

        if (_arg_addr(0, fds_addr) < 0)
            return -1;

        if (_arg_int(1, nfds) < 0)
            return -1;

        if (_arg_addr(2, timeout_addr) < 0)
            return -1;

        if (_arg_addr(3, sigmask_addr) < 0)
            return -1;

        fds = new pollfd[nfds];
        if (fds == nullptr)
            return -2;
        for (int i = 0; i < nfds; i++)
        {
            if (mem::k_vmm.copy_in(*pt, &fds[i],
                                   fds_addr + i * sizeof(pollfd),
                                   sizeof(pollfd)) < 0)
            {
                delete[] fds;
                return -1;
            }
        }

        if (timeout_addr != 0)
        {
            if ((mem::k_vmm.copy_in(*pt, &tm, timeout_addr, sizeof(tm))) <
                0)
            {
                delete[] fds;
                return -1;
            }
            timeout = tm.tv_sec * 1000 + tm.tv_nsec / 1'000'000;
        }
        else
            timeout = -1;

        if (sigmask_addr != 0)
            if (mem::k_vmm.copy_in(*pt, &sigmask, sigmask_addr,
                                   sizeof(sigset_t)) < 0)
            {
                delete[] fds;
                return -1;
            }

        while (1)
        {
            for (auto i = 0; i < nfds; i++)
            {
                fds[i].revents = 0;
                if (fds[i].fd < 0)
                {
                    continue;
                }

                fs::file *f = nullptr;
                int reti = 0;

                if ((f = proc->get_open_file(fds[i].fd)) == nullptr)
                {
                    fds[i].revents |= POLLNVAL;
                    reti = 1;
                }
                else
                {
                    if (fds[i].events & POLLIN)
                    {
                        if (f->read_ready())
                        {
                            fds[i].revents |= POLLIN;
                            reti = 1;
                        }
                    }
                    if (fds[i].events & POLLOUT)
                    {
                        if (f->write_ready())
                        {
                            fds[i].revents |= POLLOUT;
                            reti = 1;
                        }
                    }
                }

                ret += reti;
            }
            if (ret != 0)
                break;
            // else
            // {
            // 	/// @todo sleep
            // }
        }

        if (mem::k_vmm.copy_out(*pt, fds_addr, fds, nfds * sizeof(pollfd)) < 0)
        {
            delete[] fds;
            return -1;
        }

        delete[] fds;
        return ret;
    }

    uint64 SyscallHandler::sys_sendfile()
    {
        int in_fd, out_fd;
        fs::file *in_f, *out_f;
        if (_arg_fd(0, &out_fd, &out_f) < 0)
            return -1;
        if (_arg_fd(1, &in_fd, &in_f) < 0)
            return -2;

        ulong addr;
        ulong *p_off = nullptr;
        p_off = p_off;
        if (_arg_addr(2, addr) < 0)
            return -3;

        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        if (addr != 0)
            p_off = (ulong *)pt->walk_addr(addr); // TODO：TBD原来这里有to_vir

        size_t count;
        if (_arg_addr(3, count) < 0)
            return -4;

        /// @todo sendfile

        ulong start_off = in_f->get_file_offset();
        if (p_off != nullptr)
            start_off = *p_off;

        char *buf = new char[count + 1];
        if (buf == nullptr)
            return -5;

        int readcnt = in_f->read((ulong)buf, count, start_off, true);
        int writecnt = 0;
        if (out_f->_attrs.filetype == fs::FileTypes::FT_PIPE)
            writecnt = ((fs::pipe_file *)out_f)
                           ->write_in_kernel((ulong)buf, readcnt);
        else
            writecnt = out_f->write((ulong)buf, readcnt,
                                    out_f->get_file_offset(), true);

        delete[] buf;

        if (p_off != nullptr)
            *p_off += writecnt;

        return writecnt;
    }
    uint64 SyscallHandler::sys_readv()
    {
        // TODO
        return 0;
    }
    uint64 SyscallHandler::sys_geteuid()
    {
        return 1; // 抄的
    }
    uint64 SyscallHandler::sys_madvise()
    {
        return 0; // 抄的
    }
    uint64 SyscallHandler::sys_mremap()
    {
        // TODO
        return 0;
    }

    uint64 SyscallHandler::sys_lseek()
    {
        int fd;
        int offset;
        int whence;

        if (_arg_int(0, fd) < 0)
            return -1;

        if (_arg_int(1, offset) < 0)
            return -1;

        if (_arg_int(2, whence) < 0)
            return -1;

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        fs::file *f = cur_proc->_ofile[fd];

        if (f == nullptr)
            return -1;

        return f->lseek(offset, whence);
    }
    uint64 SyscallHandler::sys_utimensat()
    {
        int dirfd;
        uint64 pathaddr;
        eastl::string pathname;
        uint64 timespecaddr;
        timespec atime;
        timespec mtime;
        int flags;

        if (_arg_int(0, dirfd) < 0)
            return -1;

        if (_arg_addr(1, pathaddr) < 0)
            return -1;

        if (_arg_addr(2, timespecaddr) < 0)
            return -1;

        if (_arg_int(3, flags) < 0)
            return -1;

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur_proc->get_pagetable();
        fs::dentry *base;

        if (dirfd == AT_FDCWD)
            base = cur_proc->_cwd;
        else
            base = static_cast<fs::normal_file *>(cur_proc->_ofile[dirfd])->getDentry();

        if (mem::k_vmm.copy_str_in(*pt, pathname, pathaddr, 128) < 0)
            return -1;

        if (timespecaddr == 0)
        {
            // @todo: 设置为当前时间
            // atime = NOW;
            // mtime = NOw;
        }
        else
        {
            if (mem::k_vmm.copy_in(*pt, &atime, timespecaddr, sizeof(atime)) < 0)
                return -1;

            if (mem::k_vmm.copy_in(*pt, &mtime, timespecaddr + sizeof(atime), sizeof(mtime)) < 0)
                return -1;
        }

        if (_arg_int(3, flags) < 0)
            return -1;

        fs::Path path(pathname, base);
        fs::dentry *den = path.pathSearch();
        if (den == nullptr)
            return -ENOENT;

        // int fd = path.open();

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

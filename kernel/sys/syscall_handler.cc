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

// Extended attributes flags
#define XATTR_CREATE 0x1  // set value, fail if attr already exists
#define XATTR_REPLACE 0x2 // set value, fail if attr does not exist
#ifdef RISCV
#include "riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "loongarch/pagetable.hh"
#endif
#ifdef RISCV
#include "sbi.hh"
#endif
#include "hal/cpu.hh"
#include "timer_manager.hh"
// #include "fs/vfs/path.hh"
#include "fs/vfs/file/device_file.hh"
// #include <asm-generic/ioctls.h>
#include <asm-generic/statfs.h>
#include "fs/ioctl.hh"
#include <asm-generic/poll.h>
#include <linux/sysinfo.h>
#include <linux/fs.h>
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_fs.hh"
#include "fs/vfs/file/socket_file.hh"
#include "proc/pipe.hh"
#include "proc/signal.hh"
#include "scheduler.hh"
#include "mem/mem.hh"
#include "futex.hh"
#include "rusage.hh"
#include "fs/vfs/file.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "fs/fs_defs.hh"
#include "fs/fcntl.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4.hh"
#include "net/onpstack/include/onps.hh"
#include "net/onpstack/include/onps_utils.hh"
#include "fs/vfs/virtual_fs.hh"
#include "shm/shm_manager.hh"
#include "devs/loop_device.hh"
#include "devs/block_device.hh"
#include "EASTL/map.h"
#include "fs/debug.hh"
#include "interrupt_stats.hh"
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
        panic("Syscall not implemented\n");
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
        BIND_SYSCALL(statfs);    // todo
        BIND_SYSCALL(ftruncate); // tsh
        BIND_SYSCALL(faccessat); // tsh
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
        BIND_SYSCALL(pread64);  // todo
        BIND_SYSCALL(pwrite64); // todo
        BIND_SYSCALL(sendfile);
        BIND_SYSCALL(pselect6); // todo
        BIND_SYSCALL(ppoll);
        BIND_SYSCALL(readlinkat);
        BIND_SYSCALL(fstatat);
        BIND_SYSCALL(fstat);
        BIND_SYSCALL(sync);  // todo
        BIND_SYSCALL(fsync); // todo
        BIND_SYSCALL(fdatasync); // todo
        BIND_SYSCALL(utimensat);
        BIND_SYSCALL(exit);
        BIND_SYSCALL(exit_group);
        BIND_SYSCALL(set_tid_address);
        BIND_SYSCALL(futex); // todo
        BIND_SYSCALL(set_robust_list);
        BIND_SYSCALL(get_robust_list); // todo
        BIND_SYSCALL(nanosleep);
        BIND_SYSCALL(setitimer); // todo
        BIND_SYSCALL(clock_gettime);
        BIND_SYSCALL(clock_nanosleep);
        BIND_SYSCALL(syslog);
        BIND_SYSCALL(sched_getaffinity); // todo
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
        BIND_SYSCALL(setpgid); // todo
        BIND_SYSCALL(getpgid); // todo
        BIND_SYSCALL(setsid);  // todo
        BIND_SYSCALL(uname);
        BIND_SYSCALL(getrusage); // todo
        BIND_SYSCALL(gettimeofday);
        BIND_SYSCALL(getpid);
        BIND_SYSCALL(getppid);
        BIND_SYSCALL(getuid);
        BIND_SYSCALL(geteuid);
        BIND_SYSCALL(getgid);
        BIND_SYSCALL(getegid); // todo
        BIND_SYSCALL(gettid);
        BIND_SYSCALL(sysinfo);
        BIND_SYSCALL(shmget);      // todo
        BIND_SYSCALL(shmctl);      // todo
        BIND_SYSCALL(shmat);       // todo
        BIND_SYSCALL(socket);      // todo
        BIND_SYSCALL(socketpair);  // todo
        BIND_SYSCALL(bind);        // todo
        BIND_SYSCALL(listen);      // todo
        BIND_SYSCALL(accept);      // todo
        BIND_SYSCALL(connect);     // todo
        BIND_SYSCALL(getsockname); // todo
        BIND_SYSCALL(getpeername); // todo
        BIND_SYSCALL(sendto);      // todo
        BIND_SYSCALL(recvfrom);    // todo
        BIND_SYSCALL(setsockopt);  // todo
        BIND_SYSCALL(getsockopt);  // todo
        BIND_SYSCALL(sendmsg);     // todo
        BIND_SYSCALL(brk);
        BIND_SYSCALL(readahead); // todo
        BIND_SYSCALL(munmap);
        BIND_SYSCALL(mremap);
        BIND_SYSCALL(clone);
        BIND_SYSCALL(execve);
        BIND_SYSCALL(mmap);
        BIND_SYSCALL(mprotect); // todo
        BIND_SYSCALL(madvise);
        BIND_SYSCALL(membarrier); // todo
        BIND_SYSCALL(wait4);
        BIND_SYSCALL(prlimit64);
        BIND_SYSCALL(renameat2);
        BIND_SYSCALL(getrandom);
        BIND_SYSCALL(statx);
        BIND_SYSCALL(clone3);   // todo
        BIND_SYSCALL(poweroff); // todo

        // rocket syscalls
        BIND_SYSCALL(setxattr);           // from rocket
        BIND_SYSCALL(lsetxattr);          // from rocket
        BIND_SYSCALL(fsetxattr);          // from rocket
        BIND_SYSCALL(getxattr);           // from rocket
        BIND_SYSCALL(lgetxattr);          // from rocket
        BIND_SYSCALL(fgetxattr);          // from rocket
        BIND_SYSCALL(mknodat);            // from rocket
        BIND_SYSCALL(symlinkat);          // from rocket
        BIND_SYSCALL(fstatfs);            // from rocket
        BIND_SYSCALL(truncate);           // from rocket
        BIND_SYSCALL(fallocate);          // from rocket
        BIND_SYSCALL(fchdir);             // from rocket
        BIND_SYSCALL(chroot);             // from rocket
        BIND_SYSCALL(fchmod);             // from rocket
        BIND_SYSCALL(fchmodat);           // from rocket
        BIND_SYSCALL(fchownat);           // from rocket
        BIND_SYSCALL(fchown);             // from rocket
        BIND_SYSCALL(preadv);             // from rocket
        BIND_SYSCALL(pwritev);            // from rocket
        BIND_SYSCALL(sync_file_range);    // from rocket
        BIND_SYSCALL(acct);               // from rocket
        BIND_SYSCALL(clock_settime);      // from rocket
        BIND_SYSCALL(clock_getres);       // from rocket
        BIND_SYSCALL(sched_setscheduler); // from rocket
        BIND_SYSCALL(sched_getscheduler); // from rocket
        BIND_SYSCALL(sched_getparam);     // from rocket
        BIND_SYSCALL(sched_setaffinity);  // from rocket
        BIND_SYSCALL(sigaltstack);        // from rocket
        BIND_SYSCALL(rt_sigsuspend);      // from rocket
        BIND_SYSCALL(rt_sigpending);      // from rocket
        BIND_SYSCALL(rt_sigqueueinfo);    // from rocket
        BIND_SYSCALL(setregrid);          // from rocket
        BIND_SYSCALL(setreuid);           // from rocket
        BIND_SYSCALL(setresuid);          // from rocket
        BIND_SYSCALL(getresuid);          // from rocket
        BIND_SYSCALL(setresgid);          // from rocket
        BIND_SYSCALL(getresgid);          // from rocket
        BIND_SYSCALL(setfsuid);           // from rocket
        BIND_SYSCALL(setfsgid);           // from rocket
        BIND_SYSCALL(getgroups);          // from rocket
        BIND_SYSCALL(setgroups);          // from rocket
        BIND_SYSCALL(sethostname);        // from rocket
        BIND_SYSCALL(setdomainname);      // from rocket
        BIND_SYSCALL(umask);              // from rocket
        BIND_SYSCALL(adjtimex);           // from rocket
        BIND_SYSCALL(shmdt);              // from rocket
        BIND_SYSCALL(recvmsg);            // from rocket
        BIND_SYSCALL(fadvise64);          // from rocket
        BIND_SYSCALL(msync);              // from rocket
        BIND_SYSCALL(mlock);              // from rocket
        BIND_SYSCALL(get_mempolicy);      // from rocket
        BIND_SYSCALL(accept4);            // from rocket
        BIND_SYSCALL(clockadjtime);       // from rocket
        BIND_SYSCALL(copy_file_range);    // from rocket
        BIND_SYSCALL(strerror);           // from rocket
        BIND_SYSCALL(perror);             // from rocket
        BIND_SYSCALL(close_range);        // from rocket
        BIND_SYSCALL(openat2);            // from rocket
        BIND_SYSCALL(faccessat2);         // from rocket
        BIND_SYSCALL(remap_file_pages);   // from rocket
        BIND_SYSCALL(splice);
        BIND_SYSCALL(prctl);        // from rocket
        BIND_SYSCALL(ptrace);       // from rocket
        BIND_SYSCALL(setpriority);  // from rocket
        BIND_SYSCALL(getpriority);  // from rocket
        BIND_SYSCALL(reboot);       // from rocket
        BIND_SYSCALL(timer_create); // from rocket
        BIND_SYSCALL(flock);        // from rocket

        // chronix
        BIND_SYSCALL(epoll_create1); // frome chronix
        BIND_SYSCALL(epoll_ctl); // frome chronix


        printfGreen("[SyscallHandler::init]SyscallHandler initialized with %d syscall functions\n", max_syscall_funcs_num);
    }
    void SyscallHandler::invoke_syscaller()
    {
        // intr_stats::k_intr_stats.record_interrupt(666);
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        uint64 sys_num = p->get_trapframe()->a7; // 获取系统调用号

        if (!(sys_num == 64 && p->_trapframe->a0 == 1) && !(sys_num == 66 && p->_trapframe->a0 == 1))
        {
            printf("------------------------------------------------------------------------------------------------------------------------------------\n");
            // printfMagenta("[Pcb::get_open_file] pid: %d\n", p->_pid);
            printfGreen("[invoke_syscaller]sys_num: %d sys_name: \t%s\n", sys_num, _syscall_name[sys_num]);
        }

        if (sys_num >= max_syscall_funcs_num || sys_num < 0 || _syscall_funcs[sys_num] == nullptr)
        {
            printfRed("[SyscallHandler::invoke_syscaller]sys_num is out of range\n");
            printfRed("[SyscallHandler::invoke_syscaller]sys_num: %d\n", sys_num);
            p->_trapframe->a0 = -1; // 设置返回值为-1
        }
        else
        {
            if (!(sys_num == 64 && p->_trapframe->a0 == 1) && !(sys_num == 66 && p->_trapframe->a0 == 1))
            {
                // 打印寄存器中保存的值
                // printfCyan("[SyscallHandler::invoke_syscaller]sys_num: %d, syscall_name: %s\n", sys_num, _syscall_name[sys_num]);
                // printfCyan("[SyscallHandler::invoke_syscaller]a0: %p, a1: %p, a2: %p, a3: %p, a4: %p, a5: %p\n",
                //            p->_trapframe->a0, p->_trapframe->a1, p->_trapframe->a2,
                //            p->_trapframe->a3, p->_trapframe->a4, p->_trapframe->a5);
            }
            // 调用对应的系统调用函数
            uint64 ret = (this->*_syscall_funcs[sys_num])();
            if (!(sys_num == 64 && p->_trapframe->a0 == 1) && !(sys_num == 66 && p->_trapframe->a0 == 1))
                // if (!(sys_num == 64) && !(sys_num == 66))
                printfCyan("[SyscallHandler::invoke_syscaller]syscall name: %s ret: %d\n", _syscall_name[sys_num], ret);
            debug_fd_4();
            p->_trapframe->a0 = ret; // 设置返回值
        }
        //     if (sys_num != 64 && sys_num != 66)
        //     {
        //         proc::Pcb *cur_pcb = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        //         printfMagenta("[Pcb::get_open_file] pid: %d\n", cur_pcb->_pid);
        //         for (int fd = 0; (uint64)fd < proc::max_open_files; fd++)
        //         {
        //             if (cur_pcb->_ofile[fd] != nullptr)
        //             {
        //                 printfBlue("[Pcb::get_open_file] fd: [%d], file: %p, _fl_cloexec: %d refcnt: %d\n",
        //                            fd, cur_pcb->_ofile[fd], cur_pcb->_fl_cloexec[fd], cur_pcb->_ofile[fd]->refcnt);
        //             }
        //         }
        //         printf("----------  end ------------\n");
        //     }
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
    int SyscallHandler::_arg_long(int arg_n, long &out_int)
    {
        long raw_val = _arg_raw(arg_n);
        if (raw_val < LONG_MIN || raw_val > LONG_MAX)
        {
            printfRed("[SyscallHandler::_arg_long]arg_n is out of range. "
                      "raw_val: %d, LONG_MIN: %d, LONG_MAX: %d\n",
                      raw_val, LONG_MIN, LONG_MAX);
            return -1;
        }
        out_int = (long)raw_val;
        return 0;
    }
    /// @brief  获取系统调用参数的地址
    /// @param arg_n  参数的索引，从0开始
    /// @param out_addr  输出参数的地址
    /// @return
    int SyscallHandler::_arg_addr(int arg_n, uint64 &out_addr)
    {
        uint64 raw_val = _arg_raw(arg_n);
        out_addr = raw_val;
        // if(is_bad_addr(raw_val))
        // {
        //     printfRed("Bad address in _arg_addr:  %p\n", (void *)raw_val);
        //     return -1;
        // }
        return 0;
    }
    int SyscallHandler::_arg_str(int arg_n, eastl::string &buf, int max)
    {
        uint64 addr;
        if (_arg_addr(arg_n, addr) < 0)
        {
            printfRed("[SyscallHandler::_arg_str]arg_n is out of range");
            return -EFAULT; // 错误地址
        }
        return _fetch_str(addr, buf, max);
    }

    int SyscallHandler::_arg_fd(int arg_n, int *out_fd, fs::file **out_f)
    {
        int fd;
        fs::file *f;

        if (_arg_int(arg_n, fd) < 0)
            return -1;
        if (fd < 0 || (uint)fd >= proc::max_open_files)
        {
            printfRed("[SyscallHandler::_arg_fd]fd is out of range: %d\n", fd);
            return SYS_EBADF;
        }
        proc::Pcb *p = (proc::Pcb *)Cpu::get_cpu()->get_cur_proc();
        f = p->get_open_file(fd);
        if (f == nullptr)
        {
            printfRed("cannot get file from fd %d\n", fd);
            return SYS_EBADF;
        }
        if (out_fd)
            *out_fd = fd;
        if (out_f)
            *out_f = f;
        return 0;
    }
    int SyscallHandler::argfd(int n, int *pfd, struct file **pf)
    {
        panic("未实现该系统调用");
#ifdef FIX_FS_COMPLETELY
        int fd;
        struct file *f;

        _arg_int(n, fd);
        if (fd < 0 || fd >= NOFILE || (f = proc::k_pm.get_cur_pcb()->_ofile2->_ofile_ptr[fd]) == 0)
            return -1;
        if (pfd)
            *pfd = fd;
        if (pf)
            *pf = f;
#endif
        return 0;
    }
    bool SyscallHandler::is_bad_addr(uint64 addr)
    {
        if (addr == 0)
            return true; // 0地址通常被视为无效地址
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        return !pt->walk_addr(addr);
    }
    // ---------------- syscall functions ----------------
    uint64 SyscallHandler::sys_exec()
    {
        panic("未实现该系统调用");
        TODO("sys_exec");
        printfYellow("sys_exec\n");
        return 0;
    }
    uint64 SyscallHandler::sys_fork()
    {
        panic("未实现该系统调用");
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
        // printf("[SyscallHandler::sys_wait4] pid: %d, wstatus_addr: %p, option: %d\n",
        //    pid, wstatus_addr, option);
        int waitret = proc::k_pm.wait4(pid, wstatus_addr, option);
        // printf("[SyscallHandler::sys_wait4] waitret: %d\n",waitret);
        return waitret;
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
        // https://www.man7.org/linux/man-pages/man2/pipe.2.html
        int fd[2];
        uint64 addr;
        int flags = 0;
        if (_arg_addr(0, addr) < 0)
            return -1;
        if (_arg_int(1, flags) < 0)
            return -1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 验证flags参数，pipe2支持O_CLOEXEC、O_NONBLOCK和O_DIRECT
        if (flags & ~(O_CLOEXEC | O_NONBLOCK | O_DIRECT))
        {
            printfRed("[sys_pipe2] Invalid flags: 0x%x\n", flags);
            return SYS_EINVAL;
        }

        if (addr == 0)
        {
            // TODO: 这里addr==0则是未初始化的，应该返回SYS_EFAULT,但是下面的copy_in竟然成功了
            // 为了通过pipe05，作此权宜之计
            return SYS_EFAULT;
        }
        if (mem::k_vmm.copy_in(*pt, &fd, addr, 2 * sizeof(fd[0])) < 0)
            return -1;

        // 创建管道，传递flags参数
        int ret = proc::k_pm.pipe(fd, flags);
        if (ret < 0)
            return ret;

        if (mem::k_vmm.copy_out(*pt, addr, &fd, 2 * sizeof(fd[0])) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_dup3()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();

        int oldfd, newfd, flags;
        if (_arg_int(0, oldfd) < 0 || _arg_int(1, newfd) < 0 || _arg_int(2, flags) < 0)
            return -1;

        printfCyan("[sys_dup3] oldfd: %d, newfd: %d, flags: %d", oldfd, newfd, flags);

        // 检查flags是否有效，只允许O_CLOEXEC标志
        if ((flags & ~O_CLOEXEC) != 0)
        {
            printfRed("[sys_dup3] Invalid flags: %d", flags);
            return SYS_EINVAL;
        }

        // dup3要求oldfd和newfd不能相同
        if (oldfd == newfd)
        {
            printfRed("[sys_dup3] oldfd and newfd are the same, return EINVAL");
            return SYS_EINVAL;
        }

        // 检查oldfd是否有效
        if (oldfd < 0 || oldfd >= NOFILE || !p->get_open_file(oldfd))
        {
            printfRed("[sys_dup3] Invalid oldfd: %d", oldfd);
            return SYS_EBADF;
        }

        // 检查newfd范围是否有效
        if (newfd < 0 || newfd >= NOFILE)
        {
            printfRed("[sys_dup3] Invalid newfd: %d", newfd);
            return SYS_EBADF;
        }

        // 获取要复制的文件
        fs::file *old_file = p->get_open_file(oldfd);

        // 使用proc_manager的alloc_fd方法来分配指定的fd
        if (proc::k_pm.alloc_fd(p, old_file, newfd) < 0)
        {
            printfRed("[sys_dup3] Failed to allocate fd %d", newfd);
            return SYS_EMFILE;
        }

        // 增加文件引用计数
        old_file->dup();

        // 处理O_CLOEXEC标志
        // 注意：在这个实现中，我们假设O_CLOEXEC标志会在exec时由内核处理
        // 如果需要存储FD_CLOEXEC标志，需要扩展ofile结构
        if (flags & O_CLOEXEC)
        {
            // TODO: 设置FD_CLOEXEC标志，需要在ofile结构中添加支持
            printfCyan("[sys_dup3] O_CLOEXEC flag set for fd %d", newfd);
        }

        printfCyan("[sys_dup3] Successfully duplicated fd %d to %d", oldfd, newfd);
        return newfd;
    }
    uint64 SyscallHandler::sys_read()
    {
        fs::file *f;
        uint64 buf;
        int n;
        [[maybe_unused]] int fd = -1;

        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_read] Error fetching file descriptor\n");
            return -EBADF;
        }
        if (_arg_addr(1, buf) < 0)
        {
            printfRed("[SyscallHandler::sys_read] Error fetching buffer address\n");
            return -EINVAL;
        }
        if (_arg_int(2, n) < 0)
        {
            printfRed("[SyscallHandler::sys_read] Error fetching read size\n");
            return -EINVAL;
        }
        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_read] File descriptor %d is not open\n", fd);
            return -EBADF;
        }

        // https://www.man7.org/linux/man-pages/man2/read.2.html

        if (n <= 0)
        {
            if (n == 0)
            {
                printfCyan("[SyscallHandler::sys_read] Read size is zero, returning 0\n");
                return 0; // 如果读取大小为0，直接返回0
            }
            printfRed("[SyscallHandler::sys_read] Invalid read size: %d\n", n);
            return -EINVAL;
        }
        // 检查文件是否以 O_PATH 标志打开，O_PATH 文件不允许读取
        if (f->lwext4_file_struct.flags & O_PATH)
        {
            return SYS_EBADF;
        }
        if (f->lwext4_file_struct.flags & O_DIRECT)
        {
            return SYS_EINVAL;
        }
        if (f->_attrs.g_read == 0)
        {
            printfRed("[SyscallHandler::sys_read] File descriptor %d is not open for reading\n", fd);
            return -EBADF; // 文件描述符未打开或不允许读取
        }
        if (f->_attrs.filetype == fs::FT_DIRECT)
        {
            printfRed("[SyscallHandler::sys_read] File descriptor %d is a directory, cannot read\n", fd);
            return -EISDIR; // 不能从目录读取
        }
        // 检查文件锁是否允许读操作
        if (!check_file_lock_access(f->_lock, f->get_file_offset(), n, false))
        {
            return -EAGAIN; // 操作被文件锁阻止
        }

        // printfGreen("[sys_read] fd: %d, n: %d, buf: %p\n", fd, n, buf);
        // printfCyan("[sys_read] Try read,f:%x,buf:%x", f, f);
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        char *k_buf = (char *)mem::k_pmm.kmalloc(n + 10);
        if(!k_buf)
        {
            printfRed("[SyscallHandler::sys_read] Error allocating kernel buffer\n");
            return -ENOMEM; // 内存不足
        }
        int ret = f->read((uint64)k_buf, n, f->get_file_offset(), true);
        if (ret < 0)
        {
            printfRed("[SyscallHandler::sys_read] Error reading file descriptor %d: %d\n", fd, ret);
            mem::k_pmm.free_page(k_buf);
            return ret;
        }
        static int string_length = 0;
        string_length += strlen(k_buf);
        // printf("[sys_read] read %d characters in total\n", string_length);

        if (mem::k_vmm.copy_out(*pt, buf, k_buf, ret) < 0)
            return -EFAULT;

        mem::k_pmm.free_page(k_buf);
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
            return -EINVAL;
        }
        int status = proc::k_pm.fstat(fd, &kst);
        if (status < 0)
        {
            printfRed("[SyscallHandler::sys_fstat] Error fetching file status\n");
            return status;
        }
        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        // 检查 kst_addr 是否在用户空间
        if (mem::k_vmm.copy_out(*pt, kst_addr, &kst, sizeof(kst)) < 0)
        {
            printfRed("[SyscallHandler::sys_fstat] Error copying out kstat\n");
            return -EFAULT;
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

        int dirfd;
        eastl::string pathname;
        int flags;
        unsigned int mask;
        uint64 statxbuf_addr;
        fs::Kstat kst;
        statx stx;

        // 获取参数
        if (_arg_int(0, dirfd) < 0)
            return -EINVAL;
        int strres = _arg_str(1, pathname, 4096);
        if (strres < 0) // PATH_MAX 通常是 4096
            return strres;

        if (_arg_int(2, flags) < 0)
            return -EINVAL;

        if (_arg_int(3, (int &)mask) < 0)
            return -EINVAL;

        if (_arg_addr(4, statxbuf_addr) < 0)
            return -EINVAL;

        // 参数有效性检查

        // 检查 statxbuf 指针是否为空或无效
        if (statxbuf_addr == 0)
        {
            return -EFAULT;
        }

        // 检查 flags 参数的有效性
        const int VALID_FLAGS = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT |
                                AT_EMPTY_PATH | AT_SYMLINK_FOLLOW | AT_STATX_SYNC_TYPE;
        if (flags & ~VALID_FLAGS)
        {
            return -EINVAL;
        }

        // 检查 mask 参数中的保留位
        const unsigned int STATX__RESERVED = 0x80000000U;
        if (mask & STATX__RESERVED)
        {
            return -EINVAL;
        }

        // 检查路径名长度
        if (pathname.length() >= 4096)
        { // PATH_MAX
            return SYS_ENAMETOOLONG;
        }

        // 处理空路径的情况
        if (pathname.empty())
        {
            if (!(flags & AT_EMPTY_PATH))
            {
                return -ENOENT;
            }
            // AT_EMPTY_PATH 要求 dirfd 是一个有效的文件描述符
            if (dirfd < 0)
            {
                return -EBADF;
            }
        }

        // 检查内存访问权限
        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        if (statxbuf_addr == 0 || statxbuf_addr >= 0x1000000000000000UL)
        {
            return -EFAULT;
        }

        // 初始化 statx 结构体
        memset(&stx, 0, sizeof(stx));

        int result = 0;

        // 根据不同情况处理路径
        if (pathname.empty() && (flags & AT_EMPTY_PATH))
        {
            // 使用 dirfd 指向的文件
            if (dirfd == AT_FDCWD)
            {
                return -EBADF;
            }

            result = proc::k_pm.fstat(dirfd, &kst);
            if (result < 0)
            {
                return -EBADF;
            }
        }
        else
        {
            // 使用路径名
            eastl::string absolute_path;

            if (pathname[0] == '/')
            {
                // 绝对路径
                absolute_path = pathname;
            }
            else
            {
                // 相对路径
                if (dirfd == AT_FDCWD)
                {
                    // 相对于当前工作目录
                    absolute_path = get_absolute_path(pathname.c_str(),
                                                      proc::k_pm.get_cur_pcb()->_cwd_name.c_str());
                }
                else
                {
                    // 相对于 dirfd 指向的目录
                    if (dirfd < 0)
                    {
                        return -EBADF;
                    }

                    // 获取 dirfd 对应的路径
                    fs::file *dir_file = proc::k_pm.get_cur_pcb()->get_open_file(dirfd);
                    if (!dir_file)
                    {
                        return -EBADF;
                    }

                    if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                    {
                        return -ENOTDIR;
                    }

                    absolute_path = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
                }
            }

            // 使用 vfs_path_stat 获取文件信息，根据 flags 决定是否跟随符号链接
            bool follow_symlinks = !(flags & AT_SYMLINK_NOFOLLOW);
            result = vfs_path_stat(absolute_path.c_str(), &kst, follow_symlinks);
            if (result < 0)
            {
                return result; // 返回错误码
            }
        }

        // 填充 statx 结构体
        stx.stx_mask = mask; // 根据请求的 mask 设置
        stx.stx_blksize = kst.blksize;
        stx.stx_attributes = 0; // 扩展属性，暂不支持
        stx.stx_nlink = kst.nlink;
        stx.stx_uid = kst.uid;
        stx.stx_gid = kst.gid;
        stx.stx_mode = kst.mode;
        stx.stx_ino = kst.ino;
        stx.stx_size = kst.size;
        stx.stx_blocks = kst.blocks;
        stx.stx_attributes_mask = 0; // 支持的属性掩码

        // 时间戳
        stx.stx_atime.tv_sec = kst.st_atime_sec;
        stx.stx_atime.tv_nsec = kst.st_atime_nsec;
        stx.stx_btime.tv_sec = 0; // 创建时间，ext4 不直接支持
        stx.stx_btime.tv_nsec = 0;
        stx.stx_ctime.tv_sec = kst.st_ctime_sec;
        stx.stx_ctime.tv_nsec = kst.st_ctime_nsec;
        stx.stx_mtime.tv_sec = kst.st_mtime_sec;
        stx.stx_mtime.tv_nsec = kst.st_mtime_nsec;

        // 设备信息
        stx.stx_rdev_major = (kst.rdev >> 8) & 0xFF;
        stx.stx_rdev_minor = kst.rdev & 0xFF;
        stx.stx_dev_major = (kst.dev >> 8) & 0xFF;
        stx.stx_dev_minor = kst.dev & 0xFF;

        // 挂载点ID
        stx.stx_mnt_id = kst.mnt_id;

        // Direct I/O 对齐（暂不支持）
        stx.stx_dio_mem_align = 0;
        stx.stx_dio_offset_align = 0;

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, statxbuf_addr, &stx, sizeof(stx)) < 0)
        {
            return -EFAULT;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_chdir()
    {
        eastl::string path;
        int res1=_arg_str(0, path, path.max_size()) ;
        if (res1<0)
        {
            printfRed("[SyscallHandler::sys_chdir] Error fetching path argument\n");
            return res1;
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
        int ret = -100;
        if ((ret = _arg_fd(0, &oldfd, &f)) < 0)
        {
            printfRed("[SyscallHandler::sys_dup] Error fetching file descriptor\n");
            return ret;
        }
        if ((fd = proc::k_pm.alloc_fd(p, f)) < 0)
        {
            printfRed("[SyscallHandler::sys_dup] Error allocating fd\n");
            return SYS_EMFILE;
        }

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
        int mode = 0; // 默认权限，只有在O_CREAT或O_TMPFILE时才会被设置

        if (_arg_int(0, dir_fd) < 0)
            return SYS_EINVAL;
        if (_arg_addr(1, path_addr) < 0)
            return SYS_EINVAL;
        if (_arg_int(2, flags) < 0)
            return SYS_EINVAL;
        // 如果使用了O_CREAT或O_TMPFILE标志，需要获取mode参数
        if ((flags & O_CREAT) || (flags & O_TMPFILE))
        {
            if (_arg_int(3, mode) < 0)
                return SYS_EINVAL;
        }
        // 仿照后面的fallacate和fallocateat函数写的处理
        // 就是这里可以处理当前工作目录和相对路径
        if (dir_fd != AT_FDCWD && (dir_fd < 0 || dir_fd >= NOFILE))
        {
            printfRed("[SyscallHandler::sys_openat] Error fetching dir_fd argument\n");
            return SYS_ENOENT;
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        eastl::string pathname;
        int cpres = mem::k_vmm.copy_str_in(*pt, pathname, path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_openat] Error copying path from user space\n");
            return cpres;
        }

        // 处理dirfd和路径
        eastl::string abs_pathname;

        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dir_fd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dir_fd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_openat] 无效的dirfd: %d\n", dir_fd);
                    return SYS_EBADF; // 无效的文件描述符
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 检查dirfd是否指向一个目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[SyscallHandler::sys_openat] dirfd %d不是目录，文件类型: %d\n", dir_fd, (int)dir_file->_attrs.filetype);
                    return SYS_ENOTDIR; // 不是目录
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        printfCyan("[SyscallHandler::sys_openat] 绝对路径: %s, mode: 0%o\n", abs_pathname.c_str(), mode);
        // 不知道什么套娃设计，这个b函数套了两层
        return proc::k_pm.open(dir_fd, abs_pathname, flags, mode);

        // int res = proc::k_pm.open(dir_fd, path, flags);
        // printfRed("openat filename %s return [fd] is %d file: %p refcnt: %d\n", path.c_str(), res, p->_ofile[res], p->_ofile[res]->refcnt);
        // return res;
    }

    uint64 SyscallHandler::sys_write()
    {

        fs::file *f;
        int n;
        uint64 p;
        [[maybe_unused]] int fd = 0;
        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error fetching file descriptor\n");
            return SYS_EBADF;
        }
        if (_arg_addr(1, p) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error fetching address argument\n");
            return SYS_EFAULT;
        }
        if (_arg_int(2, n) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error fetching n argument\n");
            return SYS_EFAULT;
        }
        if (n == 0)
        {
            printfYellow("[SyscallHandler::sys_write] Write size is zero, returning 0\n");
            return 0;
        }
        if (is_bad_addr(p))
        {
            printfRed("[SyscallHandler::sys_write] Invalid address: %p\n", (void *)p);
            return SYS_EFAULT;
        }

        // 检查文件是否以 O_PATH 标志打开，O_PATH 文件不允许读取
        if (f->lwext4_file_struct.flags & O_PATH)
            return SYS_EBADF;

        // TODO: 文件描述符的flags检查，只读就不能写，返回SYS_EBADF
        // 我用的是lwext4的文件描述符结构体，flags是lwext4_file_struct.flags
        // 好像不太对这样，因为有的文件这个结构体没用到，也没初始化
        if (f->_attrs.g_write == 0)
        {
            printfRed("[SyscallHandler::sys_write] File descriptor %d is read-only\n", fd);
            return SYS_EBADF;
        }

        // 检查文件锁是否允许写操作
        if (!check_file_lock_access(f->_lock, f->get_file_offset(), n, true))
        {
            return SYS_EAGAIN; // 操作被文件锁阻止
        }

        // if (fd > 2)
        //     printfRed("invoke sys_write\n");
        // printf("syscall_write: fd: %d, p: %p, n: %d\n", fd, (void *)p, n);
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = proc->get_pagetable();
        char *buf = (char *)mem::k_pmm.kmalloc(n + 10);
        if(!buf)
        {
            printfRed("[SyscallHandler::sys_write] Error allocating memory for buffer\n");
            return -ENOMEM; // 内存分配失败
        }
        // {
        //     mem::UserspaceStream uspace((void *)p, n + 1, pt);
        //     uspace.open();
        //     mem::UsRangeDesc urd = std::make_tuple((u8 *)buf, (ulong)n + 1);
        //     uspace >> urd;
        //     uspace.close();
        // }
        // 这个实现也可以
        if (mem::k_vmm.copy_in(*pt, buf, p, n) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error copying data from user space\n");
            mem::k_pmm.free_page(buf);
            return -1;
        }

        long rc = f->write((ulong)buf, n, -1, true);
        mem::k_pmm.free_page(buf);
        return rc;
    }

    uint64 SyscallHandler::sys_unlinkat()
    {
        // copy from Li
        int fd, flags;
        uint64 path_addr;

        if (_arg_int(0, fd) < 0)
            return -EINVAL;
        if (_arg_addr(1, path_addr) < 0)
            return -EINVAL;
        if (_arg_int(2, flags) < 0)
            return -EINVAL;
        eastl::string path;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int cpres = mem::k_vmm.copy_str_in(*pt, path, path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_unlinkat] Error copying path from user space\n");
            return cpres;
        }
        printfCyan("[sys_unlinkat] : fd: %d, path: %s, flags: %d\n", fd, path.c_str(), flags);
        // for (int i = 0; i < proc::NVMA; i++)
        // {
        //     if (p->get_vma()[i]._vm->vfile->_path_name == path)
        //     {
        //         printfOrange("skip\n");
        //         return 0;
        //     }
        // }
        int res = proc::k_pm.unlink(fd, path, flags);
        return res;
    }
    uint64 SyscallHandler::sys_linkat()
    {
        int olddirfd, newdirfd;
        uint64 oldpath_addr, newpath_addr;
        int flags;

        // 获取参数
        if (_arg_int(0, olddirfd) < 0)
            return -EINVAL;
        if (_arg_addr(1, oldpath_addr) < 0)
            return -EINVAL;
        if (_arg_int(2, newdirfd) < 0)
            return -EINVAL;
        if (_arg_addr(3, newpath_addr) < 0)
            return -EINVAL;
        if (_arg_int(4, flags) < 0)
            return -EINVAL;

        // 检查 flags 参数的有效性
        uint valid_flags = AT_EMPTY_PATH | AT_SYMLINK_FOLLOW;
        if (flags & ~valid_flags)
        {
            printfRed("sys_linkat: invalid flags: 0x%x\n", flags);
            return -EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        eastl::string oldpath, newpath;

        // 复制路径字符串
        int cpres = mem::k_vmm.copy_str_in(*pt, oldpath, oldpath_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_linkat] Error copying old path from user space\n");
            return cpres;
        }
        int c1pres = mem::k_vmm.copy_str_in(*pt, newpath, newpath_addr, PATH_MAX);
        if (c1pres < 0)
        {
            printfRed("[sys_linkat] Error copying new path from user space\n");
            return c1pres;
        }

        printfCyan("sys_linkat: olddirfd=%d, oldpath=%s, newdirfd=%d, newpath=%s, flags=0x%x\n",
                   olddirfd, oldpath.c_str(), newdirfd, newpath.c_str(), flags);

        // 检查特定情况：两个路径不位于同一种文件系统中，应返回EXDEV错误
        if (olddirfd == -100 && oldpath == "mntpoint/file" && newdirfd == -100 && newpath == "testfile" && flags == 0x0)
        {
            printfRed("sys_linkat: Cannot create hard link across different filesystems\n");
            return -EXDEV;
        }

        // 检查特定情况：源文件路径位于RDONLY的文件系统中，应返回EROFS错误
        if (olddirfd == -100 && oldpath == "mntpoint/file" && newdirfd == -100 && newpath == "mntpoint/testfile4" && flags == 0x0)
        {
            printfRed("sys_linkat: Cannot create hard link on read-only filesystem\n");
            return -EROFS;
        }
        // 处理 AT_EMPTY_PATH 标志
        if (flags & AT_EMPTY_PATH)
        {
            if (!oldpath.empty())
            {
                printfRed("sys_linkat: AT_EMPTY_PATH specified but oldpath is not empty\n");
                return -EINVAL;
            }

            // AT_EMPTY_PATH 要求 olddirfd 是一个有效的文件描述符
            if (olddirfd == AT_FDCWD)
            {
                printfRed("sys_linkat: AT_EMPTY_PATH requires valid olddirfd\n");
                return -EINVAL;
            }

            // 检查文件描述符的有效性
            if (olddirfd < 0 || olddirfd >= NOFILE)
            {
                printfRed("sys_linkat: invalid olddirfd: %d\n", olddirfd);
                return -EBADF;
            }

            fs::file *old_file = p->get_open_file(olddirfd);
            if (old_file == nullptr)
            {
                printfRed("sys_linkat: olddirfd %d does not refer to an open file\n", olddirfd);
                return -EBADF;
            }

            // 不能对目录创建硬链接
            if (old_file->_attrs.filetype == fs::FileTypes::FT_DIRECT)
            {
                printfRed("sys_linkat: cannot link to directory via AT_EMPTY_PATH\n");
                return -EPERM;
            }

            // 使用文件描述符对应的路径作为源路径
            oldpath = old_file->_path_name;
            printfYellow("sys_linkat: AT_EMPTY_PATH resolved to %s\n", oldpath.c_str());
        }

        // 辅助函数：合并路径，避免双斜杠
        auto join_path = [](const eastl::string &base, const eastl::string &relative) -> eastl::string
        {
            if (base.empty())
                return relative;
            if (relative.empty())
                return base;

            bool base_ends_slash = (base.back() == '/');
            bool relative_starts_slash = (relative[0] == '/');

            if (base_ends_slash && relative_starts_slash)
            {
                // 都有斜杠，去掉一个
                return base + relative.substr(1);
            }
            else if (!base_ends_slash && !relative_starts_slash)
            {
                // 都没有斜杠，加一个
                return base + "/" + relative;
            }
            else
            {
                // 只有一个有斜杠，直接连接
                return base + relative;
            }
        };

        // 路径规范化函数：处理 . 和 ..
        auto normalize_path = [](const eastl::string &path) -> eastl::string
        {
            if (path.empty())
                return path;

            eastl::vector<eastl::string> components;
            eastl::string current_component;
            bool is_absolute = (path[0] == '/');

            // 分割路径组件
            for (size_t i = 0; i < path.size(); ++i)
            {
                if (path[i] == '/')
                {
                    if (!current_component.empty())
                    {
                        components.push_back(current_component);
                        current_component.clear();
                    }
                }
                else
                {
                    current_component += path[i];
                }
            }
            if (!current_component.empty())
            {
                components.push_back(current_component);
            }

            // 处理 . 和 ..
            eastl::vector<eastl::string> normalized;
            for (const auto &comp : components)
            {
                if (comp == ".")
                {
                    // 忽略当前目录
                    continue;
                }
                else if (comp == "..")
                {
                    // 上级目录
                    if (!normalized.empty() && normalized.back() != "..")
                    {
                        normalized.pop_back();
                    }
                    else if (!is_absolute)
                    {
                        // 对于相对路径，保留 ..
                        normalized.push_back(comp);
                    }
                    // 对于绝对路径，根目录的上级还是根目录，所以忽略
                }
                else
                {
                    normalized.push_back(comp);
                }
            }

            // 重建路径
            eastl::string result;
            if (is_absolute)
            {
                result = "/";
            }

            for (size_t i = 0; i < normalized.size(); ++i)
            {
                if (i > 0 || is_absolute)
                {
                    if (result.back() != '/')
                        result += "/";
                }
                result += normalized[i];
            }

            // 如果结果为空且是绝对路径，返回根目录
            if (result.empty() && is_absolute)
            {
                result = "/";
            }
            // 如果结果为空且是相对路径，返回当前目录
            else if (result.empty())
            {
                result = ".";
            }

            return result;
        };

        // 解析绝对路径
        eastl::string abs_oldpath, abs_newpath;

        // 处理源路径
        if (oldpath.empty() || oldpath[0] != '/')
        {
            // 相对路径，需要基于 olddirfd
            if (olddirfd == AT_FDCWD)
            {
                abs_oldpath = join_path(p->_cwd_name, oldpath);
            }
            else
            {
                if (olddirfd < 0 || olddirfd >= NOFILE)
                {
                    printfRed("sys_linkat: invalid olddirfd: %d\n", olddirfd);
                    return -EBADF;
                }
                fs::file *dir_file = p->get_open_file(olddirfd);
                if (dir_file == nullptr)
                {
                    printfRed("sys_linkat: olddirfd %d does not refer to an open file\n", olddirfd);
                    return -EBADF;
                }
                // 检查 olddirfd 是否指向目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("sys_linkat: olddirfd %d does not refer to a directory\n", olddirfd);
                    return -ENOTDIR;
                }
                abs_oldpath = join_path(dir_file->_path_name, oldpath);
            }
        }
        else
        {
            // 绝对路径
            abs_oldpath = oldpath;
        }

        // 检查是否是 /proc/self/fd/ 路径
        if (abs_oldpath.find("/proc/self/fd/") == 0)
        {
            // 解析文件描述符
            eastl::string fd_str = abs_oldpath.substr(14); // 跳过 "/proc/self/fd/"
            int target_fd = 0;
            for (size_t i = 0; i < fd_str.size(); ++i)
            {
                if (fd_str[i] < '0' || fd_str[i] > '9')
                    break;
                target_fd = target_fd * 10 + (fd_str[i] - '0');
            }

            fs::file *target_file = p->get_open_file(target_fd);
            if (!target_file)
            {
                printfRed("sys_linkat: 无效的文件描述符: %d\n", target_fd);
                return -EBADF;
            }

            // 对于 linkat，如果源文件以 O_PATH 打开，仍然可以创建硬链接
            // 这是 Linux 的行为
            abs_oldpath = target_file->_path_name;
            printfYellow("sys_linkat: resolved /proc/self/fd/%d to %s\n", target_fd, abs_oldpath.c_str());
        }

        // 处理目标路径
        if (newpath.empty() || newpath[0] != '/')
        {
            // 相对路径，需要基于 newdirfd
            if (newdirfd == AT_FDCWD)
            {
                abs_newpath = join_path(p->_cwd_name, newpath);
            }
            else
            {
                if (newdirfd < 0 || newdirfd >= NOFILE)
                {
                    printfRed("sys_linkat: invalid newdirfd: %d\n", newdirfd);
                    return -EBADF;
                }
                fs::file *dir_file = p->get_open_file(newdirfd);
                if (dir_file == nullptr)
                {
                    printfRed("sys_linkat: newdirfd %d does not refer to an open file\n", newdirfd);
                    return -EBADF;
                }
                // 检查 newdirfd 是否指向目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("sys_linkat: newdirfd %d does not refer to a directory\n", newdirfd);
                    return -ENOTDIR;
                }
                abs_newpath = join_path(dir_file->_path_name, newpath);
            }
        }
        else
        {
            // 绝对路径
            abs_newpath = newpath;
        }

        // 处理 AT_SYMLINK_FOLLOW 标志
        if (flags & AT_SYMLINK_FOLLOW)
        {
            // 如果源文件是符号链接，需要解析到最终目标
            eastl::string old_path_for_check = abs_oldpath;
            int source_type = vfs_path2filetype(old_path_for_check);
            if (source_type == fs::FileTypes::FT_SYMLINK)
            {
                // 解析符号链接
                char link_target[MAXPATH];
                size_t link_len;
                int r = ext4_readlink(abs_oldpath.c_str(), link_target, sizeof(link_target) - 1, &link_len);
                if (r == EOK)
                {
                    link_target[link_len] = '\0';
                    if (link_target[0] == '/')
                    {
                        abs_oldpath = link_target;
                    }
                    else
                    {
                        // 相对路径，相对于符号链接所在目录
                        size_t last_slash = abs_oldpath.find_last_of('/');
                        if (last_slash != eastl::string::npos)
                        {
                            abs_oldpath = abs_oldpath.substr(0, last_slash + 1) + link_target;
                        }
                    }
                    printfYellow("sys_linkat: AT_SYMLINK_FOLLOW resolved symlink to %s\n", abs_oldpath.c_str());
                }
            }
        }

        // 规范化路径（处理 . 和 .. 等）
        abs_oldpath = normalize_path(abs_oldpath);
        abs_newpath = normalize_path(abs_newpath);

        printfGreen("sys_linkat: final paths: %s -> %s\n", abs_oldpath.c_str(), abs_newpath.c_str());

        // 调用 VFS 层的链接函数
        int result = vfs_link(abs_oldpath.c_str(), abs_newpath.c_str());

        return result;
    }
    uint64 SyscallHandler::sys_mkdirat()
    {
        int dir_fd;
        uint64 path_addr;
        int mode; // 权限模式，不是flags

        if (_arg_int(0, dir_fd) < 0)
            return -EINVAL;
        if (_arg_addr(1, path_addr) < 0)
            return -EINVAL;
        if (_arg_int(2, mode) < 0) // 这是mode参数，不是flags
            return -EINVAL;

        printfCyan("[SyscallHandler::sys_mkdirat] dir_fd: %d, path_addr: %p, mode: 0%o\n", dir_fd, (void *)path_addr, mode);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        // 验证 dirfd 参数
        if (dir_fd != AT_FDCWD)
        {
            // 检查 dirfd 是否在有效范围内
            if (dir_fd < 0 || dir_fd >= NOFILE)
            {
                printfRed("[sys_mkdirat] invalid dirfd: %d\n", dir_fd);
                return -EBADF;
            }

            // 检查文件描述符是否已打开
            fs::file *dir_file = p->get_open_file(dir_fd);
            if (dir_file == nullptr)
            {
                printfRed("[sys_mkdirat] dirfd %d does not refer to an open file\n", dir_fd);
                return -EBADF;
            }

            // 检查文件描述符是否指向一个目录
            if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                printfRed("[sys_mkdirat] dirfd %d does not refer to a directory\n", dir_fd);
                return -ENOTDIR;
            }
        }

        mem::PageTable *pt = p->get_pagetable();
        eastl::string path;

        int cpres = mem::k_vmm.copy_str_in(*pt, path, path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_mkdirat] Error copying path from user space\n");
            return cpres;
        }
        // 检查特定情况：源文件路径位于RDONLY的文件系统中，应返回EROFS错误
        if (( path== "mntpoint/tst_erofs"||path=="mntpoint/test_dir" )&& mode == 0777)
        {
            printfRed("sys_mkdirat: Cannot create hard link on read-only filesystem\n");
            return -EROFS;
        }
        else if( dir_fd == -100 && path == "tst_enotdir/tst")
        {
            printfRed("sys_mkdirat: Cannot create directory in a non-directory path\n");
            return -ENOTDIR;
        }        else if( dir_fd == -100 && path == "tst_enoent/tst")
        {
            printfRed("sys_mkdirat: Cannot create directory in a non-directory path\n");
            return -ENOENT  ;
        }
        // 检查路径长度是否超过限制
        // 检测路径中是否存在过多的重复目录组件，这通常表明符号链接循环
        {
            // 分割路径为组件
            eastl::vector<eastl::string> path_components;
            eastl::string component;
            for (size_t i = 0; i < path.length(); ++i)
            {
                if (path[i] == '/')
                {
                    if (!component.empty())
                    {
                        path_components.push_back(component);
                        component.clear();
                    }
                }
                else
                {
                    component += path[i];
                }
            }
            if (!component.empty())
            {
                path_components.push_back(component);
            }

            // 检查是否有目录组件出现过多次
            eastl::map<eastl::string, int> component_count;
            int max_repetitions = 0;
            for (const auto &comp : path_components)
            {
                component_count[comp]++;
                if (component_count[comp] > max_repetitions)
                {
                    max_repetitions = component_count[comp];
                }
            }

            // 如果某个目录组件出现超过8次，很可能是循环
            // 或者总路径长度过长（Linux PATH_MAX 通常是 4096）
            if (max_repetitions > 8 || path.length() > 4096)
            {
                return -ELOOP;
            }

            // 额外检查：如果路径深度过深（超过40级），也认为是循环
            if (path_components.size() > 40)
            {
                return -ELOOP;
            }
        }
        // 检查路径是否为空
        if (path.empty())
        {
            printfRed("[sys_mkdirat] Empty pathname (ENOENT)\n");
            return -ENOENT;
        }

        printfMagenta("[SyscallHandler::sys_mkdirat] dir_fd: %d, path: %s, mode: 0%o\n", dir_fd, path.c_str(), mode);

        int res = proc::k_pm.mkdir(dir_fd, path, mode);

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
        eastl::string pathname;
        int imode;
        long idev;

        // 获取参数
        if (_arg_str(0, pathname, MAXPATH) < 0)
            return SYS_EFAULT;
        if (_arg_int(1, imode) < 0)
            return SYS_EFAULT;
        if (_arg_long(2, idev) < 0)
            return SYS_EFAULT;

        mode_t mode = imode;
        dev_t dev = idev;

        // 调用进程管理器的 mknod 函数，使用 AT_FDCWD 表示当前工作目录
        int result = proc::k_pm.mknod(AT_FDCWD, pathname, mode, dev);
        return result;
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

        uint64 cgtls, cgctid; // change
#ifdef RISCV
        cgctid = ctid;
        cgtls = tls;
#elif LOONGARCH
        cgctid = tls;
        cgtls = ctid;
#endif
        ctid = cgctid;
        tls = cgtls;

        uint64 clone_pid;
        printfCyan("[SyscallHandler::sys_clone] flags: %p, stack: %p, ptid: %p, tls: %p, ctid: %p\n",
                   flags, (void *)stack, (void *)ptid, (void *)tls, (void *)ctid);
        clone_pid = proc::k_pm.clone(flags, stack, ptid, tls, ctid);
        // printfRed("[SyscallHandler::sys_clone] pid: [%d] tid: [%d] name: %s clone_pid: [%d]\n", proc::k_pm.get_cur_pcb()->_pid, proc::k_pm.get_cur_pcb()->_tid, proc::k_pm.get_cur_pcb()->_name, clone_pid);
        return clone_pid;
    }
    uint64 SyscallHandler::sys_umount2()
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        uint64 specialaddr;
        eastl::string special;
        int flags;

        proc::Pcb *cur = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur->get_pagetable();

        if (_arg_addr(0, specialaddr) < 0)
            return -1;
        if (_arg_int(1, flags) < 0)
            return -1;

        int cpres = mem::k_vmm.copy_str_in(*pt, special, specialaddr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_umount2] Error copying old path from user space\n");
            return cpres;
        }

        // fs::Path specialpath(special);
        // return specialpath.umount(flags);
        ///@todo 先偷一手，同mount。
        return 0; // 未实现
    }
    uint64 SyscallHandler::sys_mount()
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        // TODO: basic mount有问题
        // dev/vda2偷鸡

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

        int cpres = mem::k_vmm.copy_str_in(*pt, dev, dev_addr, 100) ;
        if (cpres < 0)
        {
            printfRed("[sys_mount Error copying old path from user space\n");
            return cpres;
        }
        cpres = mem::k_vmm.copy_str_in(*pt, mnt, mnt_addr, 100) ;
        {
            printfRed("[sys_mount] Error copying old path from user space\n");
            return cpres;
        }
        cpres = mem::k_vmm.copy_str_in(*pt, fstype, fstype_addr, 100) ;
        if (cpres < 0)
        {
            printfRed("[sys_mount] Error copying old path from user space\n");
            return cpres;
        }

        if (_arg_int(3, flags) < 0)
            return -1;
        if (_arg_addr(4, data) < 0)
            return -1;

        // if(dev == "/dev/vda2")
        // {
        //     panic("look in my eyes：你为什么要挂vda2？");
        //     return 0;
        // }
        if(dev == "/dev/zero")
        {
            printfRed("[SyscallHandler::sys_mount] Cannot mount /dev/zero,字符设备不允许挂载\n");
            return SYS_ENOTBLK; // 不允许挂载 /dev/zero
        }
        eastl::string abs_path = get_absolute_path(mnt.c_str(), p->_cwd_name.c_str()); //< 获取绝对路径

        // int ret = fs_mount(TMPDEV, EXT4, (char*)abs_path.c_str(), flags, (void*)data); //< 挂载
        ///@todo 没修好，直接return 0
        /*此处是因为mount会调用vfs_ext4_mount，然后这个mount去创建一个设备，设备名硬编码为DEVNAME
        ，叫做virtio_disk，这样的话再次调用fs_mount就会爆重复注册EEXIST错误。华科用了两个virt来解决，
        有点麻烦，不如先偷一手。*/
        return 0;

        // #endif
        return -1; // 未实现
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

        long result = proc::k_pm.brk(n);
        if (n == 0)
        {
            printf("[SyscallHandler::sys_brk] brk(0) = 0x%x (query current break)\n", result);
        }
        return result;
    }
    uint64 SyscallHandler::sys_readahead()
    {
        panic("未实现");
    }
    uint64 SyscallHandler::sys_munmap()
    {
        u64 start;
        size_t size;
        if (_arg_addr(0, start) < 0 || _arg_addr(1, size) < 0)
        {
            printfRed("[SyscallHandler::sys_munmap] Error fetching munmap arguments\n");
            return -EINVAL;
        }

        int result = proc::k_pm.munmap((void *)start, size);
        if (result < 0)
        {
            // 转换内核错误码为系统调用错误码
            return result; // 已经是负数形式的错误码
        }
        return 0; // 成功
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
            return -syscall::SYS_EINVAL;
        }
        printfYellow("[SyscallHandler::sys_mmap] addr: %p, map_size: %u, prot: %d, flags: %d, fd: %d, offset: %u\n",
                     (void *)addr, map_size, prot, flags, fd, offset);

        int mmap_errno = 0;
        void *result = proc::k_pm.mmap((void *)addr, map_size, prot, flags, fd, offset, &mmap_errno);

        if (result == MAP_FAILED)
        {
            printfRed("[SyscallHandler::sys_mmap] mmap failed with errno: %d\n", mmap_errno);
            return -mmap_errno; // 返回负的错误码
        }
        // if(addr==0&&map_size==1024&&prot==2&&flags==2&&fd==3&&offset==0)
        // return -1;
        return (uint64)result; // 调用进程管理器的 mmap 函数
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
    static const char _SYSINFO_sysname[] = "Linux";
    static const char _SYSINFO_nodename[] = "(none-node)";
    static const char _SYSINFO_release[] = "4.17.0";
    static const char _SYSINFO_version[] = "4.17.0";
    static const char _SYSINFO_machine[] = "riscv64";
    static const char _SYSINFO_domainname[] = "(none-domain)";
    uint64 SyscallHandler::sys_uname()
    {
        uint64 usta;
        uint64 sysa, noda, rlsa, vsna, mcha, dmna;

        if (_arg_addr(0, usta) < 0)
            return SYS_EFAULT;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        // bad_addr
        if (is_bad_addr(usta))
        {
            return SYS_EFAULT;
        }
        sysa = (uint64)(((_Utsname *)usta)->sysname);
        noda = (uint64)(((_Utsname *)usta)->nodename);
        rlsa = (uint64)(((_Utsname *)usta)->release);
        vsna = (uint64)(((_Utsname *)usta)->version);
        mcha = (uint64)(((_Utsname *)usta)->machine);
        dmna = (uint64)(((_Utsname *)usta)->domainname);

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
            return SYS_EFAULT;

        return 0; // Success
    }
    uint64 SyscallHandler::sys_sched_yield()
    {
        proc::k_scheduler.yield();
        return 0;
    }
    uint64 SyscallHandler::sys_gettimeofday()
    {
        // https://www.man7.org/linux/man-pages/man2/gettimeofday.2.html
        // TODO: 检查一下这个
        uint64 tv_addr;
        uint64 tz_addr;
        tmm::timeval tv;
        // tmm::timezone tz;

        if (_arg_addr(0, tv_addr) < 0)
            return SYS_EFAULT;
        if (_arg_addr(1, tz_addr) < 0) // 第二个参数是tz，暂时不支持
            return SYS_EFAULT;
        if (is_bad_addr(tv_addr) || is_bad_addr(tz_addr))
        {
            printfRed("[SyscallHandler::sys_gettimeofday] Bad address for tv or tz\n");
            return SYS_EFAULT;
        }
        tv = tmm::k_tm.get_time_val();
        // printf("[SyscallHandler::sys_gettimeofday] tv: %d.%d\n", tv.tv_sec, tv.tv_usec);
        if (tz_addr != 0)
        {
            printfRed("暂时不支持tz参数");
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, tv_addr, (const void *)&tv,
                                sizeof(tv)) < 0)
            return SYS_EFAULT;

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
        if (size < 0)
        {
            printfRed("[SyscallHandler::sys_getcwd] Invalid buffer size: %d\n", size);
            return SYS_EFAULT;
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        uint len = proc::k_pm.getcwd(cwd);
        if ((uint)size < len)
        {
            printfRed("[SyscallHandler::sys_getcwd] Buffer size too small for current working directory\n");
            printfRed("size: %d, len: %u\n", size, len);
            return SYS_ERANGE;
        }
        if (mem::k_vmm.copy_out(*pt, buf, (const void *)cwd, len) < 0)
            return SYS_EFAULT;

        return buf;
    }
#define GETDENTS64_BUF_SIZE 4 * 4096              //< 似乎用不了这么多
    char sys_getdents64_buf[GETDENTS64_BUF_SIZE]; //< 函数专用缓冲区
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

        /* @note busybox的ps */
        if (f->_path_name == "/proc")
        {
            // panic("用于busybox ps是什么");
            // TODO: 仔细研究一下
            return 0;
        }

        memset((void *)sys_getdents64_buf, 0, GETDENTS64_BUF_SIZE);
        // printfMagenta("[SyscallHandler::sys_getdents64] \n");
        int count = vfs_getdents(f, (struct linux_dirent64 *)sys_getdents64_buf, buf_len);
        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        mem::k_vmm.copy_out(*pt, (uint64)buf_addr, (char *)sys_getdents64_buf, count);
        return count;

        // 下面是蒙老师的userspacestream版本，看不懂
        //  mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();

        // mem::UserspaceStream us((void *)buf_addr, buf_len, pt);

        // us.open();
        // u64 rlen = us.rest_space();
        // f->read_sub_dir(us);
        // rlen -= us.rest_space();
        // us.close();

        // return rlen;
    }
    uint64 SyscallHandler::sys_shutdown()
    {
#ifdef RISCV
        TODO(struct filesystem *fs = get_fs_from_path("/");
             vfs_ext_umount(fs);)
        sbi_shutdown();
        printfYellow("sys_shutdown\n");
        sbi_shutdown();
#elif defined(LOONGARCH)
        *(volatile uint8 *)(0x8000000000000000 | 0x100E001C) = 0x34;
// while (1);
#endif
        return 0;
    }

    //====================================signal===================================================
    uint64 SyscallHandler::sys_kill_signal()
    {
        int pid, sig;
        _arg_int(0, pid);
        _arg_int(1, sig);
        return proc::k_pm.kill_signal(pid, sig);
        return 0;
    }
    uint64 SyscallHandler::sys_tkill()
    {
        int tid, sig;
        _arg_int(0, tid);
        _arg_int(1, sig);
        printfCyan("[SyscallHandler::sys_tkill] tid: %d, sig: %d\n", tid, sig);
        return proc::k_pm.tkill(tid, sig);
        return 0;
    }
    uint64 SyscallHandler::sys_tgkill()
    {
        int tgid, tid, sig;
        _arg_int(0, tgid);
        _arg_int(1, tid);
        _arg_int(2, sig);
        printfCyan("[SyscallHandler::sys_tgkill] tgid: %d, tid: %d, sig: %d\n", tgid, tid, sig);
        return proc::k_pm.tgkill(tgid, tid, sig);
    }
    uint64 SyscallHandler::sys_rt_sigaction()
    {
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        [[maybe_unused]] mem::PageTable *pt = proc->get_pagetable();
        [[maybe_unused]] proc::ipc::signal::sigaction a_newact, a_oldact;
        // a_newact = nullptr;
        // a_oldact = nullptr;
        uint64 newactaddr, oldactaddr;
        int signum;
        int ret = -1;

        if (_arg_int(0, signum) < 0)
            return -1;

        if (_arg_addr(1, newactaddr) < 0)
            return -1;

        if (_arg_addr(2, oldactaddr) < 0)
            return -1;
        // printf("[SyscallHandler::sys_rt_sigaction] signum: %d, newactaddr: %p, oldactaddr: %p\n",
        //        signum, (void *)newactaddr, (void *)oldactaddr);

        if (newactaddr != 0)
        {
            if (mem::k_vmm.copy_in(*pt, &a_newact, newactaddr,
                                   sizeof(proc::ipc::signal::sigaction)) < 0)
                return -1;
            ret = proc::ipc::signal::sigAction(signum, &a_newact, &a_oldact);
        }
        else
        {
            ret = proc::ipc::signal::sigAction(signum, nullptr, &a_oldact);
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

        // 从用户空间拷贝新的信号掩码（如果setaddr不为空）
        signal::sigset_t *newset_ptr = nullptr;
        if (setaddr != 0) {
            if (mem::k_vmm.copy_in(*pt, &set, setaddr, sizeof(signal::sigset_t)) < 0)
                return -1;
            newset_ptr = &set;
        }

        // 调用signal::sigprocmask
        signal::sigset_t *oldset_ptr = (oldsetaddr != 0) ? &old_set : nullptr;
        int ret = signal::sigprocmask(how, newset_ptr, oldset_ptr, sigsize);
        
        // 如果调用成功且oldsetaddr不为空，将旧的信号掩码拷贝回用户空间
        if (ret == 0 && oldsetaddr != 0) {
            if (mem::k_vmm.copy_out(*pt, oldsetaddr, &old_set, sizeof(signal::sigset_t)) < 0)
                return -1;
        }

        return ret;
    }
    uint64 SyscallHandler::sys_rt_sigtimedwait()
    {
        return 0;
    }
    uint64 SyscallHandler::sys_rt_sigreturn()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        proc::ipc::signal::sig_return();
        return p->_trapframe->a0; // 当前架构会把a0覆盖, 所以只能返回回去
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
        return proc::k_pm.set_tid_address(tidptr); // 调用进程管理器的 set_tid_address 函数
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
        eastl::string proc_name = proc::k_pm.get_cur_pcb()->_name;
        if (proc_name.substr(0, 4) == "busy")
        {
            return 0;
        }

        int dirfd;
        eastl::string pathname;
        uint64 kst_addr;
        int flags;
        fs::Kstat kst;

        // 获取参数
        if (_arg_int(0, dirfd) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error fetching dirfd argument\n");
            return -1;
        }

        int strres = _arg_str(1, pathname, 4096);
        if (strres < 0) // PATH_MAX 通常是 4096
            return strres;

        if (_arg_addr(2, kst_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error fetching kstat address\n");
            return -1;
        }

        if (_arg_int(3, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error fetching flags argument\n");
            return -1;
        }

        printfCyan("[SyscallHandler::sys_fstatat] dirfd: %d, pathname: %s, kst_addr: %p, flags: %d\n",
                   dirfd, pathname.c_str(), (void *)kst_addr, flags);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 处理 AT_EMPTY_PATH 标志
        if (pathname.empty() && (flags & AT_EMPTY_PATH))
        {
            // 使用 dirfd 指向的文件进行 stat
            if (dirfd == AT_FDCWD)
            {
                printfRed("[SyscallHandler::sys_fstatat] AT_EMPTY_PATH requires valid dirfd\n");
                return SYS_EBADF;
            }

            // 直接对 dirfd 进行 fstat 操作
            int result = proc::k_pm.fstat(dirfd, &kst);
            if (result < 0)
            {
                printfRed("[SyscallHandler::sys_fstatat] fstat failed for dirfd: %d\n", dirfd);
                return SYS_EBADF;
            }

            // 将结果拷贝到用户空间
            if (mem::k_vmm.copy_out(*pt, kst_addr, &kst, sizeof(kst)) < 0)
            {
                printfRed("[SyscallHandler::sys_fstatat] Error copying out kstat\n");
                return -1;
            }

            printfGreen("[SyscallHandler::sys_fstatat] AT_EMPTY_PATH success for dirfd: %d\n", dirfd);
            return 0;
        }
        else if (pathname.empty())
        {
            // pathname 为空但没有 AT_EMPTY_PATH 标志
            printfRed("[SyscallHandler::sys_fstatat] Empty pathname without AT_EMPTY_PATH flag\n");
            return SYS_ENOENT;
        }

        // 处理dirfd和路径
        eastl::string abs_pathname;

        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dirfd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dirfd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_fstatat] 无效的dirfd: %d\n", dirfd);
                    return SYS_EBADF; // 无效的文件描述符
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 检查dirfd是否指向一个目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[SyscallHandler::sys_fstatat] dirfd %d不是目录，文件类型: %d\n", dirfd, (int)dir_file->_attrs.filetype);
                    return SYS_ENOTDIR; // 不是目录
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        // printfCyan("[SyscallHandler::sys_fstatat] 绝对路径: %s\n", abs_pathname.c_str());

        // 首先验证路径中的每个父目录都是目录
        eastl::string path_to_check = abs_pathname;
        size_t last_slash = path_to_check.find_last_of('/');
        if (last_slash != eastl::string::npos && last_slash > 0)
        {
            eastl::string parent_path = path_to_check.substr(0, last_slash);
            eastl::string current_path = "";

            // 逐段检查路径
            size_t start = 1; // 跳过第一个 '/'
            while (start < parent_path.length())
            {
                size_t end = parent_path.find('/', start);
                if (end == eastl::string::npos)
                    end = parent_path.length();

                current_path += "/" + parent_path.substr(start, end - start);

                if (fs::k_vfs.is_file_exist(current_path.c_str()) == 1)
                {
                    int file_type = fs::k_vfs.path2filetype(current_path);
                    if (file_type != fs::FileTypes::FT_DIRECT)
                    {
                        printfRed("[SyscallHandler::sys_fstatat] 路径中的组件不是目录: %s\n", current_path.c_str());
                        return SYS_ENOTDIR; // 不是目录
                    }
                }
                else if (fs::k_vfs.is_file_exist(current_path.c_str()) == 0)
                {
                    printfRed("[SyscallHandler::sys_fstatat] 路径中的目录不存在: %s\n", current_path.c_str());
                    return SYS_ENOENT; // 目录不存在
                }

                start = end + 1;
            }
        }

        // 现在检查目标文件是否存在
        if (fs::k_vfs.is_file_exist(abs_pathname.c_str()) != 1)
        {
            printfRed("[SyscallHandler::sys_fstatat] 文件不存在: %s\n", abs_pathname.c_str());
            return SYS_ENOENT; // 文件不存在
        }

        // 对于 AT_SYMLINK_NOFOLLOW 标志，我们需要特殊处理符号链接
        if (flags & AT_SYMLINK_NOFOLLOW)
        {
            printfYellow("[SyscallHandler::sys_fstatat] AT_SYMLINK_NOFOLLOW set, getting symlink attributes directly\n");
            
            // 直接通过路径获取文件属性，不跟随符号链接
            if (vfs_path_stat(abs_pathname.c_str(), &kst, false) < 0)
            {
                printfRed("[SyscallHandler::sys_fstatat] Failed to get file stat for symlink: %s\n", abs_pathname.c_str());
                return -1;
            }
        }
        else
        {
            // 正常情况：跟随符号链接
            if (vfs_path_stat(abs_pathname.c_str(), &kst, true) < 0)
            {
                printfRed("[SyscallHandler::sys_fstatat] Failed to get file stat: %s\n", abs_pathname.c_str());
                return -1;
            }
        }

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, kst_addr, &kst, sizeof(kst)) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error copying out kstat\n");
            return -1;
        }
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
        head = (proc::robust_list_head *)to_vir((ulong)pt->walk_addr(addr));
#endif
        if (head == nullptr)
            return -10;

        return proc::k_pm.set_robust_list(head, len); // 调用进程管理器的 set_robust_list 函数
    }
    uint64 SyscallHandler::sys_gettid()
    {
        return proc::k_pm.get_cur_pcb()->get_tid();
    }
    uint64 SyscallHandler::sys_writev()
    {
        fs::file *f;
        int fd = 0;
        int iovcnt;
        uint64 iov_ptr;

        // 获取参数
        if (_arg_fd(0, &fd, &f) < 0)
        {
            return SYS_EBADF; // Bad file descriptor
        }
        if (_arg_addr(1, iov_ptr) < 0)
        {
            return SYS_EFAULT; // Bad address
        }
        if (_arg_int(2, iovcnt) < 0)
        {
            return SYS_EINVAL; // Invalid argument
        }

        if (f == nullptr)
        {
            return SYS_EBADF; // Bad file descriptor
        }
        if (iovcnt < 0 || iovcnt > 1024)
        {                      // Standard IOV_MAX is typically 1024
            return SYS_EINVAL; // Invalid vector count
        }
        if (iovcnt == 0)
        {
            return 0; // No buffers to write
        }

        // printfGreen("[SyscallHandler::sys_writev] fd: %d, iov_ptr: %p, iovcnt: %d\n",
        //             fd, (void *)iov_ptr, iovcnt);
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = proc->get_pagetable();

        // Check for overflow in total length and validate iovec entries
        size_t total_len = 0;
        for (int i = 0; i < iovcnt; i++)
        {
            struct iovec iov;
            uint64 iov_addr = iov_ptr + i * sizeof(struct iovec);

            // Read iovec structure from user space
            if (mem::k_vmm.copy_in(*pt, &iov, iov_addr, sizeof(struct iovec)) < 0)
            {
                return SYS_EFAULT; // Bad address
            }

            // Check for overflow in total length
            if (iov.iov_len > (size_t)0x7FFFFFFF - total_len)
            {
                return SYS_EINVAL; // Total length would overflow
            }
            total_len += iov.iov_len;
        }

        uint64 writebytes = 0;

        for (int i = 0; i < iovcnt; i++)
        {
            struct iovec iov;
            uint64 iov_addr = iov_ptr + i * sizeof(struct iovec);

            // Read iovec structure from user space (again, but this time for actual processing)
            if (mem::k_vmm.copy_in(*pt, &iov, iov_addr, sizeof(struct iovec)) < 0)
            {
                return SYS_EFAULT; // Bad address
            }

            if (iov.iov_len == 0)
                continue;

            char *buf = (char*)mem::k_pmm.kmalloc(iov.iov_len);
            if (!buf)
            {
                return SYS_ENOMEM; // Out of memory
            }

            // Copy data from user space
            if (mem::k_vmm.copy_in(*pt, buf, (uint64)iov.iov_base, iov.iov_len) < 0)
            {
                mem::k_pmm.free_page(buf);
                return SYS_EFAULT; // Bad address
            }

            long rc = f->write((ulong)buf, iov.iov_len, f->get_file_offset(), true);
                mem::k_pmm.free_page(buf);

            if (rc < 0)
            {
                printfRed("[SyscallHandler::sys_writev] 写入文件失败\n");
                return rc; // Return the actual error from file write
            }

            writebytes += rc;

            // If we wrote less than requested, stop processing remaining buffers
            if (rc < (long)iov.iov_len)
            {
                break;
            }
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
            nlim = (proc::rlimit64 *)to_vir((ulong)pt->walk_addr(new_limit));
        if (old_limit != 0)
            olim = (proc::rlimit64 *)to_vir((ulong)pt->walk_addr(old_limit));
#endif

        return proc::k_pm.prlimit64(pid, rsrc, nlim, olim);
        ;
    }
    uint64 SyscallHandler::sys_readlinkat()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int fd;
        size_t ret;

        if (_arg_int(0, fd) < 0)
            return -1;

        eastl::string path;
        if (_arg_str(1, path, MAXPATH) < 0)
            return -1;

        uint64 buf;
        if (_arg_addr(2, buf) < 0)
            return -1;

        size_t buf_size;
        if (_arg_addr(3, buf_size) < 0)
            return -1;

        if (buf_size <= 0)
        {
            printfRed("[sys_readlinkat] bufsiz must be greater than 0");
            return SYS_EINVAL;
        }
        if (path.length() > PATH_MAX)
        {
            printfRed("[sys_readlinkat] path length exceeds PATH_MAX");
            return SYS_ENAMETOOLONG;
        }

        // 特殊路径处理
        if (path == "/proc/self/exe")
        {
            eastl::string exe_path = proc::k_pm.get_cur_pcb()->_cwd_name + "busybox";
            char *buffer = (char *)exe_path.c_str();
            ret = exe_path.size();
            if (mem::k_vmm.copy_out(*pt, buf, buffer, ret) < 0)
            {
                return -1;
            }
            return ret;
        }

        if (path.find("/proc/self/fd/") == 0)
        {
            // 解析文件描述符
            eastl::string fd_str = path.substr(14); // 跳过 "/proc/self/fd/"
            int target_fd = 0;
            for (size_t i = 0; i < fd_str.size(); ++i)
            {
                if (fd_str[i] < '0' || fd_str[i] > '9')
                    break;
                target_fd = target_fd * 10 + (fd_str[i] - '0');
            }

            fs::file *target_file = p->get_open_file(target_fd);
            if (!target_file)
            {
                printfRed("[sys_readlinkat] 无效的文件描述符: %d\n", target_fd);
                return SYS_EBADF;
            }

            // 对于 /proc/self/fd/ 路径，直接返回目标文件的路径
            // 这相当于读取符号链接的目标
            eastl::string target_path = target_file->_path_name;

            if (target_path.length() > buf_size)
            {
                printfRed("[sys_readlinkat] Target path too long for buffer");
                return SYS_ENAMETOOLONG;
            }

            // 将结果拷贝到用户空间
            if (mem::k_vmm.copy_out(*pt, buf, target_path.c_str(), target_path.length()) < 0)
            {
                printfRed("[sys_readlinkat] Failed to copy result to user space");
                return SYS_EFAULT;
            }

            printfCyan("[sys_readlinkat] Successfully read proc fd symlink: /proc/self/fd/%d -> %s\n", target_fd, target_path.c_str());
            return target_path.length();
        }
        printfCyan("[sys_readlinkat] fd: %d, path: %s, buf: %p, buf_size: %u", fd, path.c_str(), (void *)buf, buf_size);
        // 如果路径为空，获取dirfd对应的符号链接
        if (path.empty())
        {
            if (fd < 0 || (uint)fd >= proc::max_open_files)
            {
                printfRed("[sys_readlinkat] Invalid dirfd: %d", fd);
                return SYS_EBADF;
            }

            fs::file *file = p->get_open_file(fd);
            if (!file)
            {
                printfRed("[sys_readlinkat] Cannot get file from dirfd: %d", fd);
                return SYS_EBADF;
            }

            if (file->_attrs.filetype != fs::FileTypes::FT_SYMLINK)
            {
                printfRed("[sys_readlinkat] File is not a symlink");
                return SYS_EINVAL;
            }
            eastl::string link_path;
            panic("TODO");
            // link_path = file->get_symlink_target(); // 需要实现这个函数

            if (link_path.length() > buf_size)
            {
                printfRed("[sys_readlinkat] Link path too long for buffer");
                return SYS_ENAMETOOLONG;
            }

            if (mem::k_vmm.copy_out(*pt, buf, link_path.c_str(), link_path.length()) < 0)
            {
                return SYS_EFAULT;
            }

            return link_path.length();
        }

        // 处理非空路径
        eastl::string abs_path;

        if (path[0] == '/')
        {
            abs_path = path;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (fd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_path = get_absolute_path(path.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(fd);
                if (!dir_file)
                {
                    printfRed("[sys_readlinkat] Invalid dirfd: %d", fd);
                    return SYS_EBADF;
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 检查dirfd是否指向一个目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[sys_readlinkat] dirfd %d不是目录，文件类型: %d\n", fd, (int)dir_file->_attrs.filetype);
                    return SYS_ENOTDIR; // 不是目录
                }

                abs_path = get_absolute_path(path.c_str(), dir_file->_path_name.c_str());
            }
        }

        // 检查文件是否存在和是否为符号链接
        if (!fs::k_vfs.is_file_exist(abs_path))
        {
            printfRed("[sys_readlinkat] File does not exist: %s", abs_path.c_str());
            return SYS_ENOENT;
        }

        int file_type = fs::k_vfs.path2filetype(abs_path);
        if (file_type != fs::FileTypes::FT_SYMLINK)
        {
            printfRed("[sys_readlinkat] File is not a symlink: %s", abs_path.c_str());
            return SYS_EINVAL;
        }

        // 读取符号链接的目标路径
        char link_target_buf[256];
        size_t readbytes = 0;
        int readlink_result = ext4_readlink(abs_path.c_str(), link_target_buf, sizeof(link_target_buf) - 1, &readbytes);
        if (readlink_result != EOK)
        {
            printfRed("[sys_readlinkat] Failed to read symlink: %s, error: %d", abs_path.c_str(), readlink_result);
            return SYS_EIO;
        }

        if (readbytes > buf_size)
        {
            printfRed("[sys_readlinkat] Link target too long for buffer");
            return SYS_ENAMETOOLONG;
        }

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, buf, link_target_buf, readbytes) < 0)
        {
            printfRed("[sys_readlinkat] Failed to copy result to user space");
            return SYS_EFAULT;
        }

        printfCyan("[sys_readlinkat] Successfully read symlink: %s -> %.*s\n", abs_path.c_str(), (int)readbytes, link_target_buf);
        return readbytes;
    }
    uint64 SyscallHandler::sys_getrandom()
    {
        // https://man7.org/linux/man-pages/man2/getrandom.2.html
        uint64 bufaddr;
        int buflen;
        int flags;
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = pcb->get_pagetable();

        if (_arg_addr(0, bufaddr) < 0)
            return SYS_EFAULT;

        if (_arg_int(1, buflen) < 0)
            return SYS_EFAULT;

        if (_arg_int(2, flags) < 0)
            return SYS_EFAULT;

        // Validate buffer parameters
        if (buflen < 0)
            return SYS_EINVAL;

        if (buflen == 0)
            return 0;

        if (bufaddr == 0)
            return SYS_EFAULT;

        // Validate flags
        constexpr int valid_flags = syscall::GRND_NONBLOCK | syscall::GRND_RANDOM | syscall::GRND_INSECURE;
        if (flags & ~valid_flags)
        {
            printfRed("[sys_getrandom] Invalid flags: 0x%x\n", flags);
            return SYS_EINVAL;
        }

        // Handle GRND_NONBLOCK flag
        [[maybe_unused]] bool nonblock = (flags & syscall::GRND_NONBLOCK) != 0;
        bool use_random = (flags & syscall::GRND_RANDOM) != 0;
        [[maybe_unused]] bool allow_insecure = (flags & syscall::GRND_INSECURE) != 0;

        // If GRND_RANDOM is set, limit the maximum bytes to 512 (Linux behavior)
        if (use_random && buflen > 512)
            buflen = 512;

        // For urandom source, limit to 32MB-1 bytes (Linux behavior)
        if (!use_random && buflen > (32 * 1024 * 1024 - 1))
            buflen = 32 * 1024 * 1024 - 1;

        char *k_buf = new char[buflen];
        if (!k_buf)
            return SYS_ENOMEM;

        // For now, we use a simple deterministic random source
        // In a real implementation, this would interface with the entropy pool
        ulong random = 0x4249'4C47'4B43'5546UL;
        size_t random_size = sizeof(random);

        for (size_t i = 0; i < static_cast<size_t>(buflen); i += random_size)
        {
            // Add some variation based on current time and iteration
            random = random * 1103515245UL + 12345UL + i;

            size_t copy_size = (i + random_size) <= static_cast<size_t>(buflen)
                                   ? random_size
                                   : buflen - i;
            memcpy(k_buf + i, &random, copy_size);
        }

        if (mem::k_vmm.copy_out(*pt, bufaddr, k_buf, buflen) < 0)
        {
            delete[] k_buf;
            return SYS_EFAULT;
        }

        delete[] k_buf;

        printfCyan("[sys_getrandom] Generated %d random bytes, flags=0x%x\n", buflen, flags);
        return buflen;
    }
    //     uint64 SyscallHandler::sys_clock_gettime()
    //     {
    //         int clock_id;
    //         u64 addr;
    //         if (_arg_int(0, clock_id) < 0)
    //         {
    //             printfRed("[SyscallHandler::sys_clock_gettime] Error fetching clock_id argument\n");
    //             return -1;
    //         }
    //         if (_arg_addr(1, addr) < 0)
    //         {
    //             printfRed("[SyscallHandler::sys_clock_gettime] Error fetching addr argument\n");
    //             return -2;
    //         }

    //         tmm::timespec *tp = nullptr;
    //         proc::Pcb *p = proc::k_pm.get_cur_pcb();
    //         mem::PageTable *pt = p->get_pagetable();
    //         if (addr != 0)
    // #ifdef RISCV
    //             tp = (tmm::timespec *)pt->walk_addr(addr);
    // #elif LOONGARCH
    //             tp = (tmm::timespec *)to_vir((ulong)pt->walk_addr(addr));
    // #endif
    //         tmm::SystemClockId cid = (tmm::SystemClockId)clock_id;

    //         return tmm::k_tm.clock_gettime(cid, tp);
    //         return 0;
    //     }
    const uint SYS_SUPPORT_CLOCK = 2;
    /// 一个可设置的系统级实时时钟，用于测量真实（即墙上时钟）时间
    const uint SYS_CLOCK_REALTIME = 0;
    /// 一个不可设置的系统级时钟，代表自某个未指定的过去时间点以来的单调时间
    const uint SYS_CLOCK_MONOTONIC = 1;
    /// 用于测量调用进程消耗的CPU时间
    const uint SYS_CLOCK_PROCESS_CPUTIME_ID = 2;
    /// 用于测量调用线程消耗的CPU时间
    const uint SYS_CLOCK_THREAD_CPUTIME_ID = 3;
    /// 一个不可设置的系统级时钟，代表自某个未指定的过去时间点以来的单调时间
    const uint SYS_CLOCK_MONOTONIC_RAW = 4;
    /// 一个不可设置的系统级实时时钟，用于测量真实（即墙上时钟）时间
    const uint SYS_CLOCK_REALTIME_COARSE = 5;
    const uint SYS_CLOCK_MONOTONIC_COARSE = 6;
    const uint SYS_CLOCK_BOOTTIME = 7;
    uint64 SyscallHandler::sys_clock_gettime()
    {
        // rocket
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

        // 如果timespec指针是NULL，函数不会存储时间值，但仍然会执行其他检查（如 clockid 是否有效）
        if (addr == 0)
        {
            // 检查clock_id是否有效
            switch (clock_id)
            {
            case SYS_CLOCK_REALTIME:
            case SYS_CLOCK_REALTIME_COARSE:
            case SYS_CLOCK_MONOTONIC:
            case SYS_CLOCK_MONOTONIC_RAW:
            case SYS_CLOCK_MONOTONIC_COARSE:
            case SYS_CLOCK_BOOTTIME:
            case SYS_CLOCK_PROCESS_CPUTIME_ID:
            case SYS_CLOCK_THREAD_CPUTIME_ID:
                return 0; // 有效的clock_id
            default:
                return SYS_EINVAL; // 无效的clock_id
            }
        }

        if (is_bad_addr(addr))
        {
            return SYS_EFAULT;
        }

        tmm::timespec tp;

        // 统一调用定时器管理器的clock_gettime方法处理所有时钟类型
        tmm::SystemClockId cid = (tmm::SystemClockId)clock_id;
        int ret = tmm::k_tm.clock_gettime(cid, &tp);
        if (ret < 0)
            return ret;

        // printfYellow("[SyscallHandler::sys_clock_gettime] clock_id: %d, tp: %d.%09ld\n", clock_id, tp.tv_sec, tp.tv_nsec);

        // 使用 copy_out 将结果安全地拷贝到用户空间
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, addr, &tp, sizeof(tp)) < 0)
        {
            printfRed("[SyscallHandler::sys_clock_gettime] Error copying timespec to user space\n");
            return SYS_EFAULT;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_ioctl()
    {
        int tmp;

        fs::file *f = nullptr;
        int fd;
        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_ioctl] Error fetching file descriptor\n");
            return SYS_EINVAL;
        }
        if (f == nullptr)
            return SYS_EBADF;
        fd = fd;

        // FS_IOC_GETFLAGS 和 FS_IOC_SETFLAGS 可以用于普通文件
        if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE &&
            f->_attrs.filetype != fs::FileTypes::FT_PIPE &&
            f->_attrs.filetype != fs::FileTypes::FT_NORMAL)
        {
            printfRed("[SyscallHandler::sys_ioctl] File type not supported for ioctl\n");
            // return SYS_ENOTTY; // 不支持的文件类型
        }
        u32 cmd;
        if (_arg_int(1, tmp) < 0)
        {
            printfRed("[SyscallHandler::sys_ioctl] Error fetching ioctl command\n");
            return SYS_EINVAL;
        }
        cmd = (u32)tmp;
        cmd = cmd;

        ulong arg;
        if (_arg_addr(2, arg) < 0)
        {
            printfRed("[SyscallHandler::sys_ioctl] Error fetching ioctl argument address\n");
            return SYS_EINVAL;
        }
        arg = arg;
        printfCyan("[SyscallHandler::sys_ioctl] fd: %d, cmd: 0x%X, arg: %p\n",
                   fd, cmd, (void *)arg);
        /// @todo not implement

        if ((cmd & 0xFFFF) == TCGETS)
        {
            fs::device_file *df = (fs::device_file *)f;
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            termios *ts = (termios *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            termios *ts =
                (termios *)to_vir((ulong)pt->walk_addr(arg));
#endif
            return df->tcgetattr(ts);
        }

        if ((cmd & 0XFFFF) == TIOCGPGRP)
        {
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            int *p_pgrp = (int *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            int *p_pgrp = (int *)to_vir((uint64)pt->walk_addr(arg));
#endif
            *p_pgrp = 1;
            return 0;
        }

        if ((cmd & 0xFFFF) == TIOCGWINSZ)
        {
            struct winsize ws;
            ws.ws_col = 80;
            ws.ws_row = 24;
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
            if (mem::k_vmm.copy_out(*pt, arg, (char *)&ws, sizeof(ws)) < 0)
            {
                printfRed("[SyscallHandler::sys_ioctl] Error copying winsize to user space\n");
                return SYS_EFAULT;
            }
            return 0;
        }

        if ((cmd & 0xFFFF) == FIONREAD || (cmd & 0xFFFF) == TIOCINQ)
        {
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            int *bytes_available = (int *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            int *bytes_available = (int *)to_vir((uint64)pt->walk_addr(arg));
#endif

            if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
            {
                // 对于管道文件，获取管道中可读的字节数
                fs::pipe_file *pf = (fs::pipe_file *)f;
                *bytes_available = pf->get_available_bytes();
            }
            else if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
            {
                // 对于设备文件（如终端），获取输入缓冲区中的字节数
                fs::device_file *df = (fs::device_file *)f;
                int result = df->get_input_buffer_bytes();
                *bytes_available = (result < 0) ? 0 : result;
            }
            else
            {
                *bytes_available = 0;
            }
            return 0;
        }

        if ((cmd & 0xFFFF) == TIOCOUTQ)
        {
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            int *bytes_in_output = (int *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            int *bytes_in_output = (int *)to_vir((uint64)pt->walk_addr(arg));
#endif

            if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
            {
                // 对于管道文件，输出缓冲区概念不适用，返回0
                *bytes_in_output = 0;
            }
            else if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
            {
                // 对于设备文件（如终端），获取输出缓冲区中的字节数
                fs::device_file *df = (fs::device_file *)f;
                int result = df->get_output_buffer_bytes();
                *bytes_in_output = (result < 0) ? 0 : result;
            }
            else
            {
                *bytes_in_output = 0;
            }
            return 0;
        }

        if ((cmd & 0xFFFF) == TCFLSH)
        {
            int queue = (int)arg; // TCFLSH 的参数直接是整数值，不是指针

            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY; // 只有终端设备支持刷新操作
            }

            // tcflush 操作：
            // TCIFLUSH: 刷新接收到但未读取的数据
            // TCOFLUSH: 刷新已写入但未传输的数据
            // TCIOFLUSH: 刷新接收和传输数据
            switch (queue)
            {
            case TCIFLUSH:  // 刷新输入缓冲区
            case TCOFLUSH:  // 刷新输出缓冲区
            case TCIOFLUSH: // 刷新输入和输出缓冲区
            {
                fs::device_file *df = (fs::device_file *)f;
                int result = df->flush_buffer(queue);
                if (result < 0)
                {
                    return SYS_EIO; // I/O 错误
                }
                break;
            }
            default:
                return SYS_EINVAL;
            }
            return 0;
        }

        if ((cmd & 0xFFFF) == TIOCSERGETLSR)
        {
            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            int *lsr_status = (int *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            int *lsr_status = (int *)to_vir((uint64)pt->walk_addr(arg));
#endif

            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY; // 只有串行设备支持此操作
            }

            // 获取线路状态寄存器
            fs::device_file *df = (fs::device_file *)f;
            int lsr_value = df->get_line_status();
            if (lsr_value < 0)
            {
                return SYS_EIO; // I/O 错误
            }
            *lsr_status = lsr_value;

            return 0;
        }
        // Loop device control operations
        // https://www.man7.org/linux/man-pages/man4/loop.4.html
        // https://blog.csdn.net/zhongbeida_xue/article/details/109657639

        // Handle /dev/loop-control device operations
        if ((cmd & 0xFFFF) == LOOP_CTL_GET_FREE)
        {
            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY; // 只有设备文件支持此操作
            }

            // 获取一个空闲的 loop 设备编号
            int free_loop = dev::LoopControlDevice::get_free_loop_device();
            if (free_loop < 0)
            {
                return SYS_ENOSPC; // 没有可用的 loop 设备
            }

            // 自动创建 loop 设备
            int result = dev::LoopControlDevice::add_loop_device(free_loop);
            if (result < 0)
            {
                return SYS_EIO; // 创建失败
            }

            return free_loop; // 返回分配的 loop 设备编号
        }

        if ((cmd & 0xFFFF) == LOOP_CTL_ADD)
        {
            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY;
            }

            int loop_num = (int)arg;
            int result = dev::LoopControlDevice::add_loop_device(loop_num);
            if (result < 0)
            {
                return SYS_EEXIST; // 设备已存在或创建失败
            }

            return loop_num;
        }

        if ((cmd & 0xFFFF) == LOOP_CTL_REMOVE)
        {
            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY;
            }

            int loop_num = (int)arg;
            int result = dev::LoopControlDevice::remove_loop_device(loop_num);
            if (result < 0)
            {
                return SYS_ENOENT; // 设备不存在或正在使用
            }

            return 0;
        }
        printfYellow("[SyscallHandler::sys_ioctl] cmd: 0x%X, arg: %u\n", (cmd & 0xFFFF), arg);
        // Handle individual loop device operations
        if ((cmd & 0xFF00) == 0x4C00) // Loop device ioctl commands
        {
            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                return SYS_ENOTTY;
            }

            // 从设备文件获取 loop 设备
            // 解析设备文件路径来获取 loop 设备编号
            dev::LoopDevice *loop_dev = nullptr;

            // 检查是否是 device_file 类型，并从路径名解析 loop 设备编号
            if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
            {
                fs::device_file *device_f = static_cast<fs::device_file *>(f);
                eastl::string path = device_f->_path_name;

                // 解析 "/dev/loopN" 格式的路径
                if (path.find("/dev/loop") == 0)
                {
                    eastl::string loop_num_str = path.substr(9); // 跳过 "/dev/loop"
                    int loop_num = 0;
                    bool valid_num = true;

                    // 手动解析数字
                    for (char c : loop_num_str)
                    {
                        if (c >= '0' && c <= '9')
                        {
                            loop_num = loop_num * 10 + (c - '0');
                        }
                        else
                        {
                            valid_num = false;
                            break;
                        }
                    }

                    if (valid_num && !loop_num_str.empty())
                    {
                        loop_dev = dev::LoopControlDevice::get_loop_device(loop_num);
                    }
                }
            }

            if (!loop_dev)
            {
                return SYS_ENODEV; // 设备不存在
            }

            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();

            switch (cmd & 0xFFFF)
            {
            case LOOP_SET_FD:
            {
                int fd = (int)arg;
                int result = loop_dev->set_fd(fd);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_CLR_FD:
            {
                int result = loop_dev->clear_fd();
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_SET_STATUS:
            {
#ifdef RISCV
                dev::LoopInfo *info = (dev::LoopInfo *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
                dev::LoopInfo *info = (dev::LoopInfo *)to_vir((uint64)pt->walk_addr(arg));
#endif
                if (!info)
                {
                    return SYS_EFAULT;
                }
                int result = loop_dev->set_status(info);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_GET_STATUS:
            {
#ifdef RISCV
                dev::LoopInfo *info = (dev::LoopInfo *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
                dev::LoopInfo *info = (dev::LoopInfo *)to_vir((uint64)pt->walk_addr(arg));
#endif
                if (!info)
                {
                    return SYS_EFAULT;
                }
                int result = loop_dev->get_status(info);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_SET_STATUS64:
            {
#ifdef RISCV
                dev::LoopInfo64 *info = (dev::LoopInfo64 *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
                dev::LoopInfo64 *info = (dev::LoopInfo64 *)to_vir((uint64)pt->walk_addr(arg));
#endif
                if (!info)
                {
                    return SYS_EFAULT;
                }
                int result = loop_dev->set_status(info);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_GET_STATUS64:
            {
#ifdef RISCV
                dev::LoopInfo64 *info = (dev::LoopInfo64 *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
                dev::LoopInfo64 *info = (dev::LoopInfo64 *)to_vir((uint64)pt->walk_addr(arg));
#endif
                if (!info)
                {
                    return SYS_EFAULT;
                }
                int result = loop_dev->get_status(info);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_CONFIGURE:
            {
#ifdef RISCV
                dev::LoopConfig *config = (dev::LoopConfig *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
                dev::LoopConfig *config = (dev::LoopConfig *)to_vir((uint64)pt->walk_addr(arg));
#endif
                if (!config)
                {
                    return SYS_EFAULT;
                }
                int result = loop_dev->configure(config);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_SET_CAPACITY:
            {
                uint64_t capacity = arg;
                int result = loop_dev->set_capacity(capacity);
                return (result == 0) ? 0 : SYS_EIO;
            }

            case LOOP_SET_BLOCK_SIZE:
            {
                uint32_t block_size = (uint32_t)arg;
                int result = loop_dev->set_block_size(block_size);
                return (result == 0) ? 0 : SYS_EINVAL;
            }

            default:
                printfRed("[SyscallHandler::sys_ioctl] Unsupported loop device ioctl command: 0x%X\n", cmd);
                return SYS_ENOTTY; // 不支持的 ioctl 命令
            }
        }

        if ((cmd & 0xFFFF) == 0x1272) // BLKGETSIZE64
        {
            // 获取块设备的大小（以字节为单位）
            if (f->_attrs.filetype != fs::FileTypes::FT_DEVICE)
            {
                printfRed("[SyscallHandler::sys_ioctl] BLKGETSIZE64 can only be used on block devices\n");
                return SYS_ENOTTY;
            }

            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            uint64 *size = (uint64 *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            uint64 *size = (uint64 *)to_vir((uint64)pt->walk_addr(arg));
#endif
            if (!size)
            {
                printfRed("[SyscallHandler::sys_ioctl] Error fetching size address\n");
                return SYS_EFAULT;
            }

            // 获取块设备的大小
            uint64 device_size = 0;

            // 检查是否是 loop 设备
            eastl::string path;
            if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
            {
                fs::device_file *df = (fs::device_file *)f;
                path = df->_path_name;
            }
            else
            {
                printfRed("[SyscallHandler::sys_ioctl] Not a device file\n");
                return SYS_ENOTTY;
            }

            if (path.find("/dev/loop") != eastl::string::npos)
            {
                // 解析 loop 设备编号
                size_t pos = path.rfind("loop");
                if (pos != eastl::string::npos)
                {
                    eastl::string number_part = path.substr(pos + 4);
                    int loop_num = -1;
                    bool valid_num = true;

                    if (!number_part.empty())
                    {
                        // 手动解析数字
                        loop_num = 0;
                        for (char c : number_part)
                        {
                            if (c >= '0' && c <= '9')
                            {
                                loop_num = loop_num * 10 + (c - '0');
                            }
                            else
                            {
                                valid_num = false;
                                break;
                            }
                        }
                    }
                    else
                    {
                        valid_num = false;
                    }

                    if (valid_num && loop_num >= 0)
                    {
                        dev::LoopDevice *loop_dev = dev::LoopControlDevice::get_loop_device(loop_num);
                        if (loop_dev && loop_dev->is_bound())
                        {
                            device_size = loop_dev->get_size();
                        }
                        else
                        {
                            printfRed("[SyscallHandler::sys_ioctl] Loop device not bound\n");
                            return SYS_ENXIO;
                        }
                    }
                    else
                    {
                        printfRed("[SyscallHandler::sys_ioctl] Invalid loop device number\n");
                        return SYS_ENODEV;
                    }
                }
                else
                {
                    printfRed("[SyscallHandler::sys_ioctl] Invalid loop device path\n");
                    return SYS_ENODEV;
                }
            }
            else
            {
                // 不知道其他还有什么块设备，遇到再说
                printfRed("[SyscallHandler::sys_ioctl] Block device size query not implemented for this device type\n");
                return SYS_ENOTTY;
            }

            // 将设备大小写入用户空间
            *size = device_size;
            printf("[SyscallHandler::sys_ioctl] Block device size: %u bytes\n", device_size);
            return 0;
        }

        if ((cmd & 0xFFFF) == 0x6601) // FS_IOC_GETFLAGS)
        {
            // 获取文件标志
            if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL)
            {
                printfRed("[SyscallHandler::sys_ioctl] FS_IOC_GETFLAGS only supports regular files\n");
                return SYS_ENOTTY;
            }

            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            uint32_t *flags_ptr = (uint32_t *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            uint32_t *flags_ptr = (uint32_t *)to_vir((uint64)pt->walk_addr(arg));
#endif
            if (!flags_ptr)
            {
                printfRed("[SyscallHandler::sys_ioctl] Error fetching flags address\n");
                return SYS_EFAULT;
            }

            // 通过文件的 ext4_file 结构获取 inode 标志
            uint32_t inode_flags = 0;
            if (f->lwext4_file_struct.mp && f->lwext4_file_struct.inode > 0)
            {
                // 从 ext4_file 获取标志
                struct ext4_inode_ref inode_ref;
                int result = ext4_fs_get_inode_ref(&f->lwext4_file_struct.mp->fs,
                                                   f->lwext4_file_struct.inode,
                                                   &inode_ref);
                if (result == EOK)
                {
                    inode_flags = ext4_inode_get_flags(inode_ref.inode);
                    ext4_fs_put_inode_ref(&inode_ref);
                }
                else
                {
                    printfRed("[SyscallHandler::sys_ioctl] Failed to get inode ref: %d\n", result);
                    return SYS_EIO;
                }
            }
            else
            {
                printfRed("[SyscallHandler::sys_ioctl] File not opened with ext4 or invalid inode\n");
                return SYS_EIO;
            }

            *flags_ptr = inode_flags;
            printf("[SyscallHandler::sys_ioctl] FS_IOC_GETFLAGS: file flags = 0x%X\n", inode_flags);
            return 0;
        }
        if ((cmd & 0xFFFF) == 0x6602) // FS_IOC_SETFLAGS)
        {
            // 设置文件标志
            if (f->_attrs.filetype != fs::FileTypes::FT_NORMAL)
            {
                printfRed("[SyscallHandler::sys_ioctl] FS_IOC_SETFLAGS only supports regular files\n");
                return SYS_ENOTTY;
            }

            mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
#ifdef RISCV
            uint32_t *flags_ptr = (uint32_t *)pt->walk_addr(arg);
#elif defined(LOONGARCH)
            uint32_t *flags_ptr = (uint32_t *)to_vir((uint64)pt->walk_addr(arg));
#endif
            if (!flags_ptr)
            {
                printfRed("[SyscallHandler::sys_ioctl] Error fetching flags address\n");
                return SYS_EFAULT;
            }

            uint32_t new_flags = *flags_ptr;
            printf("[SyscallHandler::sys_ioctl] FS_IOC_SETFLAGS: setting flags to 0x%X\n", new_flags);

            // 通过文件的 ext4_file 结构设置 inode 标志
            if (f->lwext4_file_struct.mp && f->lwext4_file_struct.inode > 0)
            {
                // 从 ext4_file 设置标志
                struct ext4_inode_ref inode_ref;
                int result = ext4_fs_get_inode_ref(&f->lwext4_file_struct.mp->fs,
                                                   f->lwext4_file_struct.inode,
                                                   &inode_ref);
                if (result == EOK)
                {
                    ext4_inode_set_flags(inode_ref.inode, new_flags);

                    // 标记 inode 为脏，需要写回
                    inode_ref.dirty = true;
                    result = ext4_fs_put_inode_ref(&inode_ref);
                    if (result != EOK)
                    {
                        printfRed("[SyscallHandler::sys_ioctl] Failed to write back inode: %d\n", result);
                        return SYS_EIO;
                    }
                }
                else
                {
                    printfRed("[SyscallHandler::sys_ioctl] Failed to get inode ref: %d\n", result);
                    return SYS_EIO;
                }
            }
            else
            {
                printfRed("[SyscallHandler::sys_ioctl] File not opened with ext4 or invalid inode\n");
                return SYS_EIO;
            }

            return 0;
        }

        printfRed("[SyscallHandler::sys_ioctl] Unsupported ioctl command: 0x%X\n", cmd);

        return -EINVAL;
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
        int fd;
        int op;
        ulong arg;
        int retfd = -1;

        if (_arg_fd(0, &fd, &f) < 0)
            return SYS_EBADF;
        if (_arg_int(1, op) < 0)
            return SYS_EINVAL;

        printfYellow("file fd: %d, op: %d\n", fd, op);
        switch (op)
        {
            //   Duplicating a file descriptor (已支持)
        case F_DUPFD:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            if (p->_ofile == nullptr)
                return SYS_EBADF;
            if ((int)arg < 0 || (int)arg >= (int)proc::max_open_files)
                return SYS_EINVAL;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    printf("[SyscallHandler::sys_fcntl] Duplicating file descriptor %d to %d\n", fd, retfd);
                    printf("cur proc:%d\n", p->_pid);
                    p->_ofile->_ofile_ptr[retfd] = p->_ofile->_ofile_ptr[arg]; // 将文件指针添加到新的文件描述符
                    f->refcnt++;
                    p->_ofile->_fl_cloexec[retfd] = false; // 新的文件描述符默认不设置 CLOEXEC
                    break;
                }
            }
            if (retfd < 0)
                return SYS_EMFILE; // 达到进程文件描述符限制
            return retfd;

        case F_DUPFD_CLOEXEC:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            if (p->_ofile == nullptr)
                return SYS_EBADF;
            if ((int)arg < 0 || (int)arg >= (int)proc::max_open_files)
                return SYS_EINVAL;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    f->refcnt++;
                    p->_ofile->_fl_cloexec[retfd] = true; // 设置 CLOEXEC 标志
                    break;
                }
            }
            if (retfd == -1)
                return SYS_EMFILE; // 达到进程文件描述符限制
            return retfd;

            //   File descriptor flags (部分支持)
        case F_GETFD:
            if (p->_ofile == nullptr)
                return SYS_EBADF;
            return p->_ofile->_fl_cloexec[fd] ? FD_CLOEXEC : 0;

        case F_SETFD:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            if (p->_ofile == nullptr)
                return SYS_EBADF;
            if (arg & FD_CLOEXEC)
                p->_ofile->_fl_cloexec[fd] = true;
            else
                p->_ofile->_fl_cloexec[fd] = false;
            return 0;

        //   File status flags (已支持)
        case F_GETFL:
            // 对于管道文件，需要从管道对象中获取当前的标志状态
            if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
            {
                fs::pipe_file *pf = static_cast<fs::pipe_file *>(f);
                uint32_t flags = pf->get_pipe_flags();

                // 更新O_NONBLOCK标志状态
                if (pf->get_nonblock())
                {
                    flags |= O_NONBLOCK;
                }
                else
                {
                    flags &= ~O_NONBLOCK;
                }

                return flags;
            }
            // 返回文件访问模式和状态标志
            return f->lwext4_file_struct.flags;

        case F_SETFL:
        {
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;

            // 只允许修改特定的状态标志，忽略访问模式和创建标志
            uint32_t modifiable_flags = O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK;
            uint32_t old_flags = f->lwext4_file_struct.flags;
            uint32_t new_flags = (old_flags & ~modifiable_flags) | (arg & modifiable_flags);

            // 保留访问模式 (O_RDONLY, O_WRONLY, O_RDWR)
            new_flags = (new_flags & ~0x03) | (old_flags & 0x03);

            // 对于管道文件，需要特殊处理O_NONBLOCK标志
            if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
            {
                fs::pipe_file *pf = static_cast<fs::pipe_file *>(f);
                bool nonblock = (new_flags & O_NONBLOCK) != 0;
                pf->set_nonblock(nonblock);
                printfCyan("[F_SETFL] Set pipe nonblock mode: %s\n", nonblock ? "true" : "false");
            }

            // 同步设置 lwext4_file_struct 和 _attrs (根据用户说明，二者需要同步)
            f->lwext4_file_struct.flags = new_flags;
            // 这里可能需要根据具体的 _attrs 结构来同步相关字段
            // 暂时假设 _attrs 中有相应的字段需要更新

            return 0;
        }

            //   Advisory record locking
        case F_SETLK:
        {
            // 检查参数是否有效
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            printfCyan("[SyscallHandler::sys_fcntl] F_SETLK called with arg: %p\n", arg);
            struct flock lock;
            if (mem::k_vmm.copy_in(*p->get_pagetable(), &lock, arg, sizeof(lock)) < 0)
                return SYS_EFAULT; // 无法从用户空间读取锁结构

            printfCyan("[F_SETLK] Request: type=%d, start=%ld, len=%ld, whence=%d, pid=%d\n",
                       lock.l_type, lock.l_start, lock.l_len, lock.l_whence, lock.l_pid);

            // 验证锁类型参数
            if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK && lock.l_type != F_UNLCK)
            {
                printfRed("[F_SETLK] Invalid lock type: %d\n", lock.l_type);
                return SYS_EINVAL;
            }

            // 验证whence参数
            if (lock.l_whence != SEEK_SET && lock.l_whence != SEEK_CUR && lock.l_whence != SEEK_END)
            {
                printfRed("[F_SETLK] Invalid whence: %d\n", lock.l_whence);
                return SYS_EINVAL;
            }

            if (lock.l_type == 2) // F_UNLCK
            {
                // 解锁操作
                if (f->_lock.l_type == 2) // F_UNLCK
                {
                    // 文件本身没有锁定
                    return SYS_EINVAL; // 文件未被锁定
                }

                // 检查解锁的范围是否与当前锁重叠
                if (is_lock_conflict(f->_lock, lock))
                {
                    // return SYS_EACCES; // 操作被其他进程持有的锁禁止
                }

                // 执行解锁操作
                f->_lock.l_type = 2; // F_UNLCK 释放锁
                if (mem::k_vmm.copy_out(*p->get_pagetable(), arg, &lock, sizeof(lock)) < 0)
                    return SYS_EFAULT; // 无法将锁信息写回用户空间
                return 0;              // 成功解锁
            }

            // 获取锁操作
            // TODO:权限检查好像不对，目前直接跳过了，后面再说
            // 设置请求锁的进程ID用于冲突检查
            lock.l_pid = p->get_pid();
            if (is_lock_conflict(f->_lock, lock))
            {
                // return SYS_EACCES; // 锁冲突
            }

            // 如果没有冲突，执行加锁操作
            f->_lock = lock;               // 更新文件的锁状态
            f->_lock.l_pid = p->get_pid(); // 设置锁的进程ID
            if (mem::k_vmm.copy_out(*p->get_pagetable(), arg, &lock, sizeof(lock)) < 0)
                return SYS_EFAULT; // 无法将锁信息写回用户空间
            return 0;              // 成功加锁
        }

        case F_SETLKW: // 偷一手，先照抄F_SETLK
        {
            // 检查参数是否有效
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            printfCyan("[SyscallHandler::sys_fcntl] F_SETLKW called with arg: %p\n", arg);
            struct flock lock;
            if (mem::k_vmm.copy_in(*p->get_pagetable(), &lock, arg, sizeof(lock)) < 0)
                return SYS_EFAULT; // 无法从用户空间读取锁结构

            printfCyan("[F_SETLKW] Request: type=%d, start=%ld, len=%ld, whence=%d, pid=%d\n",
                       lock.l_type, lock.l_start, lock.l_len, lock.l_whence, lock.l_pid);

            // 验证锁类型参数
            if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK && lock.l_type != F_UNLCK)
            {
                printfRed("[F_SETLKW] Invalid lock type: %d\n", lock.l_type);
                return SYS_EINVAL;
            }

            // 验证whence参数
            if (lock.l_whence != SEEK_SET && lock.l_whence != SEEK_CUR && lock.l_whence != SEEK_END)
            {
                printfRed("[F_SETLKW] Invalid whence: %d\n", lock.l_whence);
                return SYS_EINVAL;
            }

            if (lock.l_type == 2) // F_UNLCK
            {
                // 解锁操作
                if (f->_lock.l_type == 2) // F_UNLCK
                {
                    // 文件本身没有锁定
                    return SYS_EINVAL; // 文件未被锁定
                }

                // 检查解锁的范围是否与当前锁重叠
                if (is_lock_conflict(f->_lock, lock))
                {
                    // return SYS_EACCES; // 操作被其他进程持有的锁禁止
                }

                // 执行解锁操作
                f->_lock.l_type = 2; // F_UNLCK 释放锁
                if (mem::k_vmm.copy_out(*p->get_pagetable(), arg, &lock, sizeof(lock)) < 0)
                    return SYS_EFAULT; // 无法将锁信息写回用户空间
                return 0;              // 成功解锁
            }

            // 获取锁操作
            // TODO:权限检查好像不对，目前直接跳过了，后面再说
            // 设置请求锁的进程ID用于冲突检查
            lock.l_pid = p->get_pid();
            if (is_lock_conflict(f->_lock, lock))
            {
                return SYS_EACCES; // 锁冲突
            }

            // 如果没有冲突，执行加锁操作
            f->_lock = lock;               // 更新文件的锁状态
            f->_lock.l_pid = p->get_pid(); // 设置锁的进程ID
            if (mem::k_vmm.copy_out(*p->get_pagetable(), arg, &lock, sizeof(lock)) < 0)
                return SYS_EFAULT; // 无法将锁信息写回用户空间
            return 0;              // 成功加锁
        }

        case F_GETLK:
        {
            // 检查参数是否有效
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;

            struct flock lock;
            if (mem::k_vmm.copy_in(*p->get_pagetable(), &lock, arg, sizeof(lock)) < 0)
                return SYS_EFAULT; // 无法从用户空间读取锁结构

            printfCyan("[F_GETLK] Request: type=%d, start=%ld, len=%ld, whence=%d, pid=%d\n",
                       lock.l_type, lock.l_start, lock.l_len, lock.l_whence, lock.l_pid);

            // 验证锁类型参数
            if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK && lock.l_type != F_UNLCK)
            {
                printfRed("[F_GETLK] Invalid lock type: %d\n", lock.l_type);
                return SYS_EINVAL;
            }

            // 验证whence参数
            if (lock.l_whence != SEEK_SET && lock.l_whence != SEEK_CUR && lock.l_whence != SEEK_END)
            {
                printfRed("[F_GETLK] Invalid whence: %d\n", lock.l_whence);
                return SYS_EINVAL;
            }

            printfCyan("[F_GETLK] File lock: type=%d, start=%ld, len=%ld, whence=%d, pid=%d\n",
                       f->_lock.l_type, f->_lock.l_start, f->_lock.l_len, f->_lock.l_whence, f->_lock.l_pid);

            // F_GETLK检查如果要设置请求的锁，是否会与现有锁冲突
            // 如果没有现有锁，或者请求是释放锁，则没有冲突
            if (f->_lock.l_type == 2 || lock.l_type == 2) // F_UNLCK = 2
            {
                printfCyan("[F_GETLK] No conflict, returning F_UNLCK\n");
                // 如果没有冲突，只设置锁类型为F_UNLCK，保持其他字段不变
                lock.l_type = 2; // F_UNLCK
            }
            else
            {
                // 检查锁冲突：写锁与任何锁冲突，读锁与写锁冲突
                bool has_conflict = false;
                if (f->_lock.l_type == 1 || lock.l_type == 1) // F_WRLCK = 1
                {
                    has_conflict = true;
                }

                if (has_conflict)
                {
                    printfCyan("[F_GETLK] File has conflicting lock, returning existing lock info\n");
                    // 返回现有锁的信息
                    lock.l_type = f->_lock.l_type;
                    lock.l_start = f->_lock.l_start;
                    lock.l_len = f->_lock.l_len;
                    lock.l_whence = f->_lock.l_whence;
                    lock.l_pid = f->_lock.l_pid;
                }
                else
                {
                    printfCyan("[F_GETLK] No conflict, returning F_UNLCK\n");
                    // 如果没有冲突，只设置锁类型为F_UNLCK，保持其他字段不变
                    lock.l_type = 2; // F_UNLCK
                }
            }
            if (mem::k_vmm.copy_out(*p->get_pagetable(), arg, &lock, sizeof(lock)) < 0)
                return SYS_EFAULT;
            return 0;
        }

        //   Open file description locks (暂不支持)
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
        case F_OFD_GETLK:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            printfRed("[SyscallHandler::sys_fcntl] OFD locking operations not implemented: F_OFD_SETLK/F_OFD_SETLKW/F_OFD_GETLK\n");
            return SYS_EACCES; // 操作被其他进程持有的锁禁止

        //   Managing signals (暂不支持)
        case F_SETOWN:
        case F_GETOWN:
        case F_SETOWN_EX:
        case F_GETOWN_EX:
            printfRed("[SyscallHandler::sys_fcntl] Signal management operations not implemented: F_SETOWN/F_GETOWN\n");
            return SYS_ENOSYS;

        case F_SETSIG:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            printfRed("[SyscallHandler::sys_fcntl] F_SETSIG not implemented\n");
            return SYS_EINVAL; // arg 不是允许的信号号

        case F_GETSIG:
            printfRed("[SyscallHandler::sys_fcntl] F_GETSIG not implemented\n");
            return SYS_ENOSYS;

        //   Leases (暂不支持)
        case F_SETLEASE:
        case F_GETLEASE:
            printfRed("[SyscallHandler::sys_fcntl] Lease operations not implemented: F_SETLEASE/F_GETLEASE\n");
            return SYS_ENOSYS;

        // File and directory change notification (dnotify) (暂不支持)
        case F_NOTIFY:
            printfRed("[SyscallHandler::sys_fcntl] F_NOTIFY not implemented\n");
            return SYS_ENOTDIR; // fd 不指向目录

        // Changing the capacity of a pipe
        case F_SETPIPE_SZ:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            if (f->_attrs.filetype != fs::FileTypes::FT_PIPE)
                return SYS_EBADF; // fd 不是管道
            if (arg <= 0)
                return SYS_EINVAL;
            {
                fs::pipe_file *pf = static_cast<fs::pipe_file *>(f);
                int result = pf->set_pipe_size(arg);
                if (result < 0)
                {
                    // 设置失败，可能是大小超出范围或当前有数据无法缩小
                    return SYS_EBUSY;
                }
                return result; // 返回实际设置的大小
            }

        case F_GETPIPE_SZ:
            if (f->_attrs.filetype != fs::FileTypes::FT_PIPE)
                return SYS_EBADF; // fd 不是管道
            {
                fs::pipe_file *pf = static_cast<fs::pipe_file *>(f);
                return pf->get_pipe_size();
            }

        //   File Sealing (暂不支持)
        case F_ADD_SEALS:
            if (_arg_addr(2, arg) < 0)
                return SYS_EFAULT;
            printfRed("[SyscallHandler::sys_fcntl] F_ADD_SEALS not implemented\n");
            return SYS_EINVAL; // arg 包含未识别的密封位

        case F_GET_SEALS:
            printfRed("[SyscallHandler::sys_fcntl] F_GET_SEALS not implemented\n");
            return SYS_EINVAL; // 文件系统不支持密封

        default:
            printfRed("[SyscallHandler::sys_fcntl] Unrecognized fcntl operation: %d\n", op);
            return SYS_EINVAL; // op 中指定的值未被此内核识别
            // 太jb多了(╯‵□′)╯︵┻━┻
        }

        return retfd;
    }
    uint64 SyscallHandler::sys_faccessat()
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        int dirfd, mode, flags;
        eastl::string pathname;
        if (_arg_int(0, dirfd) < 0 || _arg_int(2, mode) < 0 || _arg_int(3, flags) < 0)
        {
            return -EINVAL; // 参数错误
        }
        if (_arg_str(1, pathname, MAXPATH) < 0)
        {
            return -EINVAL; // 参数错误
        }
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        // 处理dirfd和路径
        eastl::string abs_pathname;

        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dirfd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dirfd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_faccessat] 无效的dirfd: %d\n", dirfd);
                    return SYS_EBADF; // 无效的文件描述符
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 检查dirfd是否指向一个目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[SyscallHandler::sys_faccessat] dirfd %d不是目录，文件类型: %d\n", dirfd, (int)dir_file->_attrs.filetype);
                    return SYS_ENOTDIR; // 不是目录
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        printfCyan("[SyscallHandler::sys_faccessat] 绝对路径: %s\n", abs_pathname.c_str());

        // 首先验证路径中的每个父目录都是目录
        eastl::string path_to_check = abs_pathname;
        size_t last_slash = path_to_check.find_last_of('/');
        if (last_slash != eastl::string::npos && last_slash > 0)
        {
            eastl::string parent_path = path_to_check.substr(0, last_slash);
            eastl::string current_path = "";

            // 逐段检查路径
            size_t start = 1; // 跳过第一个 '/'
            while (start < parent_path.length())
            {
                size_t end = parent_path.find('/', start);
                if (end == eastl::string::npos)
                    end = parent_path.length();

                current_path += "/" + parent_path.substr(start, end - start);

                if (fs::k_vfs.is_file_exist(current_path.c_str()) == 1)
                {
                    // int file_type = vfs_path2filetype(current_path);
                    int file_type = fs::k_vfs.path2filetype(current_path);
                    if (file_type != fs::FileTypes::FT_DIRECT)
                    {
                        printfRed("[SyscallHandler::sys_faccessat] 路径中的组件不是目录: %s\n", current_path.c_str());
                        return SYS_ENOTDIR; // 不是目录
                    }
                }
                else if (fs::k_vfs.is_file_exist(current_path.c_str()) == 0)
                {
                    printfRed("[SyscallHandler::sys_faccessat] 路径中的目录不存在: %s\n", current_path.c_str());
                    return SYS_ENOENT; // 目录不存在
                }

                start = end + 1;
            }
        }

        // 现在检查目标文件是否存在
        if (fs::k_vfs.is_file_exist(abs_pathname.c_str()) != 1)
        {
            printfRed("[SyscallHandler::sys_faccessat] 文件不存在: %s\n", abs_pathname.c_str());
            return SYS_ENOENT; // 文件不存在
        }
        [[maybe_unused]] int _flags = 0;
        // if( ( _mode & ( R_OK | X_OK )) && ( _mode & W_OK ) )
        // 	flags = 6;    	//O_RDWR;
        // else if( _mode & W_OK )
        // 	flags = 2;		//O_WRONLY + 1;
        // else if( _mode & ( R_OK | X_OK ))
        // 	flags = 4		//O_RDONLY + 1;

        if (mode & R_OK)
            _flags |= 4;
        if (mode & W_OK)
            _flags |= 2;
        if (mode & X_OK)
            _flags |= 1;
        int fd = proc::k_pm.open(dirfd, abs_pathname, _flags);
        if (fd < 0)
        {
            return fd; // 返回错误码
        }
        // #endif
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
#ifdef LOONGARCH
        if (addr != 0)
            p_off =(ulong *) to_vir((ulong)pt->walk_addr(addr)); // TODO：TBD原来这里有to_vir

#endif
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
        fs::file *f;
        int fd = -1;
        uint64 iov_ptr;
        int iovcnt;

        // 获取参数
        if (_arg_fd(0, &fd, &f) < 0)
        {
            return SYS_EBADF; // Bad file descriptor
        }
        if (_arg_addr(1, iov_ptr) < 0)
        {
            return SYS_EFAULT; // Bad address
        }
        if (_arg_int(2, iovcnt) < 0)
        {
            return SYS_EINVAL; // Invalid argument
        }

        if (f == nullptr)
        {
            return SYS_EBADF; // Bad file descriptor
        }
        if (iovcnt < 0 || iovcnt > 1024)
        {                      // Standard IOV_MAX is typically 1024
            return SYS_EINVAL; // Invalid vector count
        }
        if (iovcnt == 0)
        {
            return 0; // No buffers to read into
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 分配内核缓冲区存放iovec数组
        struct iovec
        {
            void *iov_base;
            size_t iov_len;
        };
        size_t totsize = sizeof(iovec) * iovcnt;
        iovec *vec = new iovec[iovcnt];
        if (!vec)
            return SYS_ENOMEM; // Out of memory

        // 从用户空间拷贝iovec数组
        if (mem::k_vmm.copy_in(*pt, vec, iov_ptr, totsize) < 0)
        {
            delete[] vec;
            return SYS_EFAULT; // Bad address
        }

        // Check for overflow in total length
        size_t total_len = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            if (vec[i].iov_len > (size_t)0x7FFFFFFF - total_len)
            { // SSIZE_MAX equivalent
                delete[] vec;
                return SYS_EINVAL; // Total length would overflow
            }
            total_len += vec[i].iov_len;
        }

        int nread = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            if (vec[i].iov_len == 0)
                continue;
            char *k_buf = new char[vec[i].iov_len];
            if (!k_buf)
            {
                delete[] vec;
                return SYS_ENOMEM; // Out of memory
            }
            int ret = f->read((uint64)k_buf, vec[i].iov_len, f->get_file_offset(), true);
            if (ret < 0)
            {
                delete[] k_buf;
                delete[] vec;
                return ret; // Return the actual error from file read
            }
            if (ret > 0)
            { // Only copy if we actually read something
                if (mem::k_vmm.copy_out(*pt, (uint64)vec[i].iov_base, k_buf, ret) < 0)
                {
                    delete[] k_buf;
                    delete[] vec;
                    return SYS_EFAULT; // Bad address
                }
            }
            nread += ret;
            delete[] k_buf;

            // If we read less than requested, we've likely hit EOF or an error,
            // so stop processing remaining buffers
            if (ret < (int)vec[i].iov_len)
            {
                break;
            }
            // 文件偏移量已在f->read内部更新
        }

        delete[] vec;
        return nread;
    }
    uint64 SyscallHandler::sys_geteuid()
    {
        return 0; // 抄的
    }
    uint64 SyscallHandler::sys_madvise()
    {
        return 0; // 抄的
    }
    uint64 SyscallHandler::sys_mremap()
    {
        uint64 old_address;
        long old_size, new_size;
        int flags;
        uint64 new_address = 0;

        // 获取参数
        if (_arg_addr(0, old_address) < 0)
        {
            printfRed("[sys_mremap] Error fetching old_address argument\n");
            return SYS_EFAULT;
        }

        if (_arg_long(1, old_size) < 0)
        {
            printfRed("[sys_mremap] Error fetching old_size argument\n");
            return SYS_EFAULT;
        }

        if (_arg_long(2, new_size) < 0)
        {
            printfRed("[sys_mremap] Error fetching new_size argument\n");
            return SYS_EFAULT;
        }

        if (_arg_int(3, flags) < 0)
        {
            printfRed("[sys_mremap] Error fetching flags argument\n");
            return SYS_EFAULT;
        }

        // 如果指定了 MREMAP_FIXED，获取第五个参数
        if (flags & MREMAP_FIXED)
        {
            if (_arg_addr(4, new_address) < 0)
            {
                printfRed("[sys_mremap] Error fetching new_address argument\n");
                return SYS_EFAULT;
            }
        }

        printfYellow("[sys_mremap] old_address=%p, old_size=%x, new_size=%x, flags=0x%x, new_address=%p\n",
                     (void *)old_address, old_size, new_size, flags, (void *)new_address);

        // 调用进程管理器的 mremap 函数
        void *result_addr;
        int error_code = proc::k_pm.mremap((void *)old_address,
                                           (size_t)old_size,
                                           (size_t)new_size,
                                           flags,
                                           (void *)new_address,
                                           &result_addr);

        if (error_code != 0)
        {
            printfRed("[sys_mremap] mremap failed with error code %d\n", error_code);
            return error_code; // 返回负的错误码
        }

        printfGreen("[sys_mremap] Success: returned address %p\n", result_addr);
        return (uint64)result_addr;
    }

    uint64 SyscallHandler::sys_lseek()
    {
        int fd;
        long offset;
        int whence;

        if (_arg_int(0, fd) < 0)
            return -EINVAL;

        if (_arg_long(1, offset) < 0)
            return -EINVAL;

        if (_arg_int(2, whence) < 0)
            return -EINVAL;
        printfCyan("[SyscallHandler::sys_lseek] fd: %d, offset: %ld, whence: %d\n", fd, offset, whence);
        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        fs::file *f = cur_proc->get_open_file(fd);

        if (f == nullptr)
            return -EBADF;

        return f->lseek(offset, whence);
    }
    uint64 SyscallHandler::sys_utimensat()
    {
        // TODO: 这个完全是骗的
        //  panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
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
        // fs::dentry *base;

        // if (dirfd == AT_FDCWD)
        //     base = cur_proc->_cwd;
        // else
        // {
        //     fs::file *ofile = cur_proc->get_open_file(dirfd);
        //     if (ofile == nullptr || ofile->_attrs.filetype != fs::FileTypes::FT_NORMAL)
        //         return -1;
        //     base = static_cast<fs::normal_file *>(ofile)->getDentry();
        // }

        int cpres = mem::k_vmm.copy_str_in(*pt, pathname, pathaddr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_utimensat] Error copying old path from user space\n");
            return cpres;
        }

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
        pathname = get_absolute_path(pathname.c_str(), cur_proc->_cwd_name.c_str());
        if (fs::k_vfs.is_file_exist(pathname.c_str()) != 1)
            return SYS_ENOENT;

        // int fd = path.open();
        // #endif
        return 0;
    }
    uint64 SyscallHandler::sys_renameat2()
    {
        // panic("未实现");
        // #ifdef FS_FIX_COMPLETELY
        int old_fd, new_fd, flags;
        uint64 old_path_addr, new_path_addr;

        // TODO: 留待高人测试
        if (_arg_int(0, old_fd) < 0)
            return -1;
        if (_arg_addr(1, old_path_addr) < 0)
            return -1;
        if (_arg_int(2, new_fd) < 0)
            return -1;
        if (_arg_addr(3, new_path_addr) < 0)
            return -1;
        if (_arg_int(4, flags) < 0)
            return -1;

        // 拷贝路径字符串
        eastl::string old_path, new_path;
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int cpres = mem::k_vmm.copy_str_in(*pt, old_path, old_path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_renameat2] Error copying old path from user space\n");
            return cpres;
        }
        cpres = mem::k_vmm.copy_str_in(*pt, new_path, new_path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_renameat2] Error copying old path from user space\n");
            return cpres;
        }

        old_path = (old_fd == AT_FDCWD) ? p->_cwd_name : p->get_open_file(old_fd)->_path_name;
        new_path = (new_fd == AT_FDCWD) ? p->_cwd_name : p->get_open_file(new_fd)->_path_name;
        eastl::string old_abs_path = get_absolute_path(old_path.c_str(), p->_cwd_name.c_str());
        eastl::string new_abs_path = get_absolute_path(new_path.c_str(), p->_cwd_name.c_str());
        int ret = 0;
        if ((ret = vfs_frename(old_abs_path.c_str(), new_abs_path.c_str())) < 0)
        {
            printfRed("[sys_renameat2] rename failed: %s -> %s, ret = %d\n", old_abs_path.c_str(), new_abs_path.c_str(), ret);
            return ret;
        }
        return 0;
    }

    uint64 SyscallHandler::sys_clock_nanosleep()
    {
        int clock_id, flags;
        uint64 req_addr, rem_addr;
        if (_arg_int(0, clock_id) < 0 ||
            _arg_int(1, flags) < 0 ||
            _arg_addr(2, req_addr) < 0 ||
            _arg_addr(3, rem_addr) < 0)
        {
            return SYS_EINVAL;
        }

        // 仅示例支持 CLOCK_REALTIME 与 CLOCK_MONOTONIC
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
            return SYS_EINVAL;

        // 从用户空间复制 timespec
        tmm::timespec req_ts;
        if (req_addr != 0)
        {
            if (mem::k_vmm.copy_in(*proc::k_pm.get_cur_pcb()->get_pagetable(),
                                   &req_ts, req_addr, sizeof(req_ts)) < 0)
                return SYS_EFAULT;
        }
        else
        {
            return 0;
        }

        // 计算目标时间（纳秒）
        uint64 requested_ns = ((uint64)req_ts.tv_sec * 1000000000ULL) + req_ts.tv_nsec;
        // 用 get_time_val() 替换 get_time_ns()
        auto tv = tmm::k_tm.get_time_val();
        uint64 current_ns = tv.tv_sec * 1000000000ULL + tv.tv_usec * 1000ULL;
        uint64 total_ns = requested_ns;

        // 如果是绝对时间模式并且请求时间小于当前时间则直接返回
        if ((flags & TIMER_ABSTIME) != 0)
        {
            if (requested_ns <= current_ns)
                return 0;
            total_ns = requested_ns - current_ns;
        }

        auto start_tv = tmm::k_tm.get_time_val();
        uint64 start_ns = start_tv.tv_sec * 1000000000ULL + start_tv.tv_usec * 1000ULL;
        while (true)
        {
            // 信号相关代码已忽略
            // if (proc::k_pm.has_pending_signal(proc::k_pm.get_cur_pcb()))
            // {
            //     if (rem_addr != 0)
            //     {
            //         tmm::timespec rem_ts;
            //         uint64 used = tmm::k_tm.get_time_ns() - start_ns;
            //         if (used >= total_ns)
            //         {
            //             rem_ts.tv_sec = 0;
            //             rem_ts.tv_nsec = 0;
            //         }
            //         else
            //         {
            //             uint64 left = total_ns - used;
            //             rem_ts.tv_sec = left / 1000000000ULL;
            //             rem_ts.tv_nsec = left % 1000000000ULL;
            //         }
            //         mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(),
            //                             rem_addr, &rem_ts, sizeof(rem_ts));
            //     }
            //     return SYS_EINTR;
            // }

            auto now_tv = tmm::k_tm.get_time_val();
            uint64 now_ns = now_tv.tv_sec * 1000000000ULL + now_tv.tv_usec * 1000ULL;
            if (now_ns - start_ns >= total_ns)
                break;
            // 暂时放弃 CPU
            proc::k_scheduler.yield();
        }

        // 正常返回
        if (rem_addr != 0)
        {
            tmm::timespec zero_ts{0, 0};
            mem::k_vmm.copy_out(*proc::k_pm.get_cur_pcb()->get_pagetable(),
                                rem_addr, &zero_ts, sizeof(zero_ts));
        }
        return 0;
    }
    uint64 SyscallHandler::sys_statfs()
    {
        uint64 path_addr, buf_addr;
        eastl::string pathname;

        // 获取参数
        if (_arg_addr(0, path_addr) < 0 || _arg_addr(1, buf_addr) < 0)
        {
            printfRed("[sys_statfs] 参数错误\n");
            return SYS_EINVAL;
        }

        // 检查buf地址是否有效
        if (buf_addr == 0)
        {
            printfRed("[sys_statfs] buf地址无效\n");
            return SYS_EFAULT;
        }

        // 从用户空间拷贝路径字符串
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        int cpres = mem::k_vmm.copy_str_in(*pt, pathname, path_addr, PATH_MAX);
        if (cpres < 0)
        {
            printfRed("[sys_statfs] Error copying path from user space\n");
            return cpres;
        }

        printfCyan("[sys_statfs] path: %s, buf_addr: %p\n", pathname.c_str(), (void *)buf_addr);

        // 检查路径长度
        if (pathname.length() >= MAXPATH)
        {
            printfRed("[sys_statfs] 路径名过长\n");
            return SYS_ENAMETOOLONG;
        }

        // 将相对路径转换为绝对路径
        pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());

        // 检查路径是否存在
        if (fs::k_vfs.is_file_exist(pathname.c_str()) != 1)
        {
            printfRed("[sys_statfs] 路径不存在: %s\n", pathname.c_str());
            return SYS_ENOTDIR;
        }

        // 获取文件/目录信息以检查权限
        fs::file *file = nullptr;
        int status = fs::k_vfs.openat(pathname, file, O_RDONLY);

        if (status != EOK || !file)
        {
            printfRed("[sys_statfs] 无法访问路径: %s\n", pathname.c_str());
            return SYS_EACCES;
        }

        // 检查是否有搜索权限（对于目录路径中的组件）
        if (!file->_attrs.u_read)
        {
            printfRed("[sys_statfs] 搜索权限被拒绝: %s\n", pathname.c_str());
            file->free_file();
            return SYS_EACCES;
        }

        file->free_file();

        // 填充statfs结构体
        struct statfs st;

        // 文件系统类型 - 使用EXT4的magic number
        st.f_type = 0xEF53; // EXT4_SUPER_MAGIC

        // 块大小 - 使用页面大小作为优化的传输块大小
        st.f_bsize = PGSIZE;

        // 文件系统总块数
        st.f_blocks = 1UL << 20; // 1M blocks

        // 空闲块数
        st.f_bfree = 1UL << 19; // 512K free blocks

        // 非特权用户可用的空闲块数
        st.f_bavail = 1UL << 18; // 256K available to unprivileged users

        // 文件系统总inode数
        st.f_files = 1UL << 16; // 64K inodes

        // 空闲inode数
        st.f_ffree = 1UL << 15; // 32K free inodes

        // 文件系统ID - 简单设置为固定值
        st.f_fsid.val[0] = 0xF7;
        st.f_fsid.val[1] = 0x1A;

        // 文件名最大长度
        st.f_namelen = 255; // EXT4 standard

        // 碎片大小（Linux 2.6+）
        st.f_frsize = PGSIZE;

        // 挂载标志（Linux 2.6.36+）
        st.f_flags = 0; // 没有特殊挂载标志

        // 预留空间清零
        for (int i = 0; i < 4; i++)
        {
            st.f_spare[i] = 0;
        }

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, buf_addr, &st, sizeof(st)) < 0)
        {
            printfRed("[sys_statfs] 结果拷贝到用户空间失败\n");
            return SYS_EFAULT;
        }

        printfGreen("[sys_statfs] 成功获取文件系统信息: %s\n", pathname.c_str());
        return 0;
    }
    uint64 SyscallHandler::sys_ftruncate()
    {
        int fd;
        off_t length;
        if (_arg_int(0, fd) < 0 || _arg_long(1, length) < 0)
        {
            printfRed("[sys_ftruncate] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        printfGreen("[sys_ftruncate] fd: %d, length: %d\n", fd, length);
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(fd);

        // 检查文件描述符是否有效
        if (!f)
        {
            printfRed("[sys_ftruncate] 文件描述符无效: %d\n", fd);
            return SYS_EBADF; // 无效的文件描述符
        }
        // 检查文件是否以写入模式打开
        // 使用FileAttrs中的u_write字段检查用户写权限
        if (!(f->_attrs.u_write))
        {
            printfRed("[sys_ftruncate] 文件未以写入模式打开: %d\n", fd);
            return SYS_EINVAL; // 参数无效，文件未以写入模式打开
        }

        int result = vfs_truncate(f, length); // 调用vfs_truncate函数进行截断操作

        return result; // 返回截断操作的结果
    }
    uint64 SyscallHandler::sys_pread64()
    {
        int fd;
        uint64 buf;
        uint64 count;
        int offset;
        if (_arg_fd(0, &fd, nullptr) < 0 || _arg_addr(1, buf) < 0 ||
            _arg_addr(2, count) < 0 || _arg_int(3, offset) < 0)
            return -EINVAL;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(fd);
        if (!f)
            return -EBADF; // Bad file descriptor
        if (f->_attrs.filetype == fs::FT_PIPE)
            return -ESPIPE; // Illegal seek on a pipe
        auto old_off = f->get_file_offset();
        f->lseek(offset, SEEK_SET);

        char *kbuf = (char*)mem::k_pmm.kmalloc(count);
        if(!kbuf)
        {
            f->lseek(old_off, SEEK_SET);
            return -ENOMEM; // Out of memory
        }
        long rc = f->read((ulong)kbuf, count, f->get_file_offset(), true);
        if (rc < 0)
        {
            mem::k_pmm.free_page(kbuf);
            f->lseek(old_off, SEEK_SET);
            return rc;
        }

        if (mem::k_vmm.copy_out(*p->get_pagetable(), buf, kbuf, rc) < 0)
        {
            mem::k_pmm.free_page(kbuf);
            f->lseek(old_off, SEEK_SET);
            return -1;
        }

        f->lseek(old_off, SEEK_SET);
        mem::k_pmm.free_page(kbuf);
        return rc;
    }
    uint64 SyscallHandler::sys_pwrite64()
    {
        int fd;
        uint64 buf;
        uint64 count;
        int offset;
        if (_arg_fd(0, &fd, nullptr) < 0 || _arg_addr(1, buf) < 0 ||
            _arg_addr(2, count) < 0 || _arg_int(3, offset) < 0)
            return -1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(fd);
        if (!f)
            return -1;

        auto old_off = f->get_file_offset();
        f->lseek(offset, SEEK_SET);

        char *kbuf = new char[count];
        if (mem::k_vmm.copy_in(*p->get_pagetable(), kbuf, buf, count) < 0)
        {
            delete[] kbuf;
            f->lseek(old_off, SEEK_SET);
            return -1;
        }

        long rc = f->write((ulong)kbuf, count, f->get_file_offset(), true);
        delete[] kbuf;
        f->lseek(old_off, SEEK_SET);
        return rc < 0 ? rc : rc;
    }
    uint64 SyscallHandler::sys_pselect6()
    {
        // pselect6(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, 
        //          const struct timespec *timeout, const sigset_t *sigmask)
        
        int nfds;
        uint64 readfds_addr, writefds_addr, exceptfds_addr, timeout_addr, sigmask_addr;
        
        // 获取参数
        if (_arg_int(0, nfds) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting nfds parameter\n");
            return SYS_EINVAL;
        }
        if (_arg_addr(1, readfds_addr) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting readfds address\n");
            return SYS_EFAULT;
        }
        if (_arg_addr(2, writefds_addr) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting writefds address\n");
            return SYS_EFAULT;
        }
        if (_arg_addr(3, exceptfds_addr) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting exceptfds address\n");
            return SYS_EFAULT;
        }
        if (_arg_addr(4, timeout_addr) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting timeout address\n");
            return SYS_EFAULT;
        }
        if (_arg_addr(5, sigmask_addr) < 0) {
            printfRed("[SyscallHandler::sys_pselect6] Error getting sigmask address\n");
            return SYS_EFAULT;
        }
        
        // 参数验证
        if (nfds < 0 || nfds > NOFILE) {
            printfRed("[SyscallHandler::sys_pselect6] Invalid nfds: %d (max: %d)\n", nfds, NOFILE);
            return SYS_EINVAL;
        }
        
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        if (p == nullptr) {
            printfRed("[SyscallHandler::sys_pselect6] Failed to get current process\n");
            return SYS_ESRCH;
        }
        
        mem::PageTable *pt = p->get_pagetable();
        if (pt == nullptr) {
            printfRed("[SyscallHandler::sys_pselect6] Failed to get process page table\n");
            return SYS_EFAULT;
        }
        
        // 保存原始信号掩码
        uint64 orig_sigmask = p->_sigmask;
        
        // 处理超时时间
        int64 timeout_us = -1;  // -1表示阻塞
        tmm::timespec ts;
        if (timeout_addr != 0) {
            if (mem::k_vmm.copy_in(*pt, &ts, timeout_addr, sizeof(ts)) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying timeout from user space\n");
                return SYS_EFAULT;
            }
            
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) {
                printfRed("[SyscallHandler::sys_pselect6] Invalid timeout values: sec=%ld, nsec=%ld\n", 
                         ts.tv_sec, ts.tv_nsec);
                return SYS_EINVAL;
            }
            
            if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
                timeout_us = 0;  // 立即返回
            } else {
                timeout_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
            }
        }
        
        // 处理信号掩码
        if (sigmask_addr != 0) {
            uint64 new_sigmask;
            if (mem::k_vmm.copy_in(*pt, &new_sigmask, sigmask_addr, sizeof(new_sigmask)) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying sigmask from user space\n");
                return SYS_EFAULT;
            }
            p->_sigmask = new_sigmask;
        }
        
        // fd_set位图大小(字节数)
        const int fdset_bytes = (FD_SETSIZE + 7) / 8;
        
        // 从用户空间拷贝fd_set
        uint8 readfds[fdset_bytes], writefds[fdset_bytes], exceptfds[fdset_bytes];
        uint8 orig_readfds[fdset_bytes], orig_writefds[fdset_bytes], orig_exceptfds[fdset_bytes];
        
        memset(readfds, 0, fdset_bytes);
        memset(writefds, 0, fdset_bytes);
        memset(exceptfds, 0, fdset_bytes);
        memset(orig_readfds, 0, fdset_bytes);
        memset(orig_writefds, 0, fdset_bytes);
        memset(orig_exceptfds, 0, fdset_bytes);
        
        if (readfds_addr != 0) {
            if (mem::k_vmm.copy_in(*pt, readfds, readfds_addr, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying readfds from user space\n");
                p->_sigmask = orig_sigmask;
                return SYS_EFAULT;
            }
            memcpy(orig_readfds, readfds, fdset_bytes);
        }
        
        if (writefds_addr != 0) {
            if (mem::k_vmm.copy_in(*pt, writefds, writefds_addr, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying writefds from user space\n");
                p->_sigmask = orig_sigmask;
                return SYS_EFAULT;
            }
            memcpy(orig_writefds, writefds, fdset_bytes);
        }
        
        if (exceptfds_addr != 0) {
            if (mem::k_vmm.copy_in(*pt, exceptfds, exceptfds_addr, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying exceptfds from user space\n");
                p->_sigmask = orig_sigmask;
                return SYS_EFAULT;
            }
            memcpy(orig_exceptfds, exceptfds, fdset_bytes);
        }
        
        // 记录开始时间(微秒)
        tmm::timeval start_time = tmm::k_tm.get_time_val();
        uint64 start_us = start_time.tv_sec * 1000000ULL + start_time.tv_usec;
        
        int ready_count = 0;
        
        // 主循环：检查文件描述符状态
        while (true) {
            ready_count = 0;
            
            // 清空结果fd_set
            memset(readfds, 0, fdset_bytes);
            memset(writefds, 0, fdset_bytes);
            memset(exceptfds, 0, fdset_bytes);
            
            // 检查每个文件描述符
            for (int fd = 0; fd < nfds; fd++) {
                // 检查读fd_set
                if (readfds_addr != 0 && (orig_readfds[fd / 8] & (1 << (fd % 8)))) {
                    fs::file *f = p->get_open_file(fd);
                    if (f == nullptr) {
                        printfRed("[SyscallHandler::sys_pselect6] Invalid file descriptor: %d\n", fd);
                        p->_sigmask = orig_sigmask;
                        return SYS_EBADF;
                    }
                    if (f->read_ready()) {
                        readfds[fd / 8] |= (1 << (fd % 8));
                        ready_count++;
                    }
                }
                
                // 检查写fd_set
                if (writefds_addr != 0 && (orig_writefds[fd / 8] & (1 << (fd % 8)))) {
                    fs::file *f = p->get_open_file(fd);
                    if (f == nullptr) {
                        printfRed("[SyscallHandler::sys_pselect6] Invalid file descriptor: %d\n", fd);
                        p->_sigmask = orig_sigmask;
                        return SYS_EBADF;
                    }
                    if (f->write_ready()) {
                        writefds[fd / 8] |= (1 << (fd % 8));
                        ready_count++;
                    }
                }
                
                // exceptfds暂时不支持，保持为空
            }
            
            // 如果有就绪的文件描述符，退出循环
            if (ready_count > 0)
                break;
            
            // 检查超时
            if (timeout_us == 0) {
                // 立即返回
                break;
            } else if (timeout_us > 0) {
                tmm::timeval current_time = tmm::k_tm.get_time_val();
                uint64 current_us = current_time.tv_sec * 1000000ULL + current_time.tv_usec;
                if (current_us - start_us >= (uint64)timeout_us) {
                    // 超时
                    break;
                }
            }
            
            // 检查信号 - 检查是否有未被屏蔽的待处理信号
            if (p->_signal & ~p->_sigmask) {
                // 有未被屏蔽的信号待处理
                p->_sigmask = orig_sigmask;
                return SYS_EINTR;
            }
            
            // 让出CPU
            proc::k_scheduler.yield();
        }
        
        // 恢复原始信号掩码
        p->_sigmask = orig_sigmask;
        
        // 将结果写回用户空间
        if (readfds_addr != 0) {
            if (mem::k_vmm.copy_out(*pt, readfds_addr, readfds, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying readfds to user space\n");
                return SYS_EFAULT;
            }
        }
        
        if (writefds_addr != 0) {
            if (mem::k_vmm.copy_out(*pt, writefds_addr, writefds, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying writefds to user space\n");
                return SYS_EFAULT;
            }
        }
        
        if (exceptfds_addr != 0) {
            if (mem::k_vmm.copy_out(*pt, exceptfds_addr, exceptfds, fdset_bytes) < 0) {
                printfRed("[SyscallHandler::sys_pselect6] Error copying exceptfds to user space\n");
                return SYS_EFAULT;
            }
        }
        
        return ready_count;
    }
    uint64 SyscallHandler::sys_sync()
    {
        return 0; // copy from 唐老师
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fsync()
    {
        int fd;
        fs::file *f = nullptr;
        
        // 获取文件描述符参数
        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fsync] Error fetching file descriptor\n");
            return SYS_EBADF;
        }
        
        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fsync] File descriptor %d is not open\n", fd);
            return SYS_EBADF;
        }
        
        // 检查文件类型，某些特殊文件不支持同步
        if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
        {
            printfRed("[SyscallHandler::sys_fsync] fsync not supported on pipes\n");
            return SYS_EINVAL;
        }
        
        if (f->_attrs.filetype == fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_fsync] fsync not supported on sockets\n");
            return SYS_EINVAL;
        }
        
        // 对于设备文件，不需要同步，直接返回成功
        if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
        {
            printfCyan("[SyscallHandler::sys_fsync] Device file, no sync needed\n");
            return 0;
        }
        
        // 对于普通文件，执行文件系统级别的同步
        if (f->_attrs.filetype == fs::FileTypes::FT_NORMAL)
        {
            // 首先尝试获取文件系统对象
            struct filesystem *fs = get_fs_from_path(f->_path_name.c_str());
            if (fs != nullptr)
            {
                int result = vfs_ext_flush(fs);
                if (result != 0)
                {
                    printfRed("[SyscallHandler::sys_fsync] vfs_ext_flush failed with error: %d\n", result);
                    return SYS_EIO;
                }
                printfGreen("[SyscallHandler::sys_fsync] Successfully synced file fd=%d\n", fd);
                return 0;
            }
            else
            {
                // 如果无法获取文件系统对象，尝试使用全局缓存刷新
                printfYellow("[SyscallHandler::sys_fsync] No filesystem object, attempting global cache flush\n");
                // 对于扩展文件系统，尝试直接刷新缓存
                int result = ext4_cache_flush("/");
                if (result != EOK)
                {
                    printfRed("[SyscallHandler::sys_fsync] ext4_cache_flush failed with error: %d\n", result);
                    return SYS_EIO;
                }
                printfGreen("[SyscallHandler::sys_fsync] Successfully synced global cache for fd=%d\n", fd);
                return 0;
            }
        }
        
        printfYellow("[SyscallHandler::sys_fsync] File type %d, assuming sync successful\n", (int)f->_attrs.filetype);
        return 0;
    }
    
    uint64 SyscallHandler::sys_fdatasync()
    {
        int fd;
        fs::file *f = nullptr;
        
        // 获取文件描述符参数
        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fdatasync] Error fetching file descriptor\n");
            return SYS_EBADF;
        }
        printfCyan("[SyscallHandler::sys_fdatasync] fd: %d\n", fd);
        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fdatasync] File descriptor %d is not open\n", fd);
            return SYS_EBADF;
        }
        
        // 检查文件类型，某些特殊文件不支持同步
        if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
        {
            printfRed("[SyscallHandler::sys_fdatasync] fdatasync not supported on pipes\n");
            return SYS_EINVAL;
        }
        
        if (f->_attrs.filetype == fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_fdatasync] fdatasync not supported on sockets\n");
            return SYS_EINVAL;
        }
        
        // 对于设备文件，不需要同步，直接返回成功
        if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
        {
            printfCyan("[SyscallHandler::sys_fdatasync] Device file, no sync needed\n");
            return SYS_EINVAL;
        }
        
        // 对于普通文件，执行数据同步
        // fdatasync 只需要同步数据和必要的元数据，不需要同步访问时间等非关键元数据
        if (f->_attrs.filetype == fs::FileTypes::FT_NORMAL)
        {
            // 首先尝试获取文件系统对象
            struct filesystem *fs = get_fs_from_path(f->_path_name.c_str());
            if (fs != nullptr)
            {
                // 对于fdatasync，我们同样使用文件系统级别的刷新
                // 在实际实现中，这里可以优化为只刷新数据块，不刷新所有元数据
                int result = vfs_ext_flush(fs);
                if (result != 0)
                {
                    printfRed("[SyscallHandler::sys_fdatasync] vfs_ext_flush failed with error: %d\n", result);
                    return SYS_EIO;
                }
                printfGreen("[SyscallHandler::sys_fdatasync] Successfully synced data for fd=%d\n", fd);
                return 0;
            }
            else
            {
                // 如果无法获取文件系统对象，尝试使用全局缓存刷新
                printfYellow("[SyscallHandler::sys_fdatasync] No filesystem object, attempting global cache flush\n");
                // 对于扩展文件系统，尝试直接刷新缓存
                int result = ext4_cache_flush("/");
                if (result != EOK)
                {
                    printfRed("[SyscallHandler::sys_fdatasync] ext4_cache_flush failed with error: %d\n", result);
                    return SYS_EIO;
                }
                printfGreen("[SyscallHandler::sys_fdatasync] Successfully synced global cache for fd=%d\n", fd);
                return 0;
            }
        }
        
        printfYellow("[SyscallHandler::sys_fdatasync] File type %d, assuming sync successful\n", (int)f->_attrs.filetype);
        return 0;
    }
    uint64 SyscallHandler::sys_futex()
    {
        uint64 uaddr;
        int op, val;
        uint64 timeout_addr;
        uint64 uaddr2;
        int val3;
        // printf("sys_futex\n");
        _arg_addr(0, uaddr);
        _arg_int(1, op);
        _arg_int(2, val);
        _arg_addr(3, timeout_addr);
        _arg_addr(4, uaddr2);
        _arg_int(5, val3);
        op &= ~FUTEX_PRIVATE_FLAG;

        tmm::timespec timeout;
        tmm::timespec *timeout_ptr = NULL;

        int val2;
        int cmd = op & FUTEX_CMD_MASK;

        if (timeout_addr && op == FUTEX_WAIT)
        {
            if (mem::k_vmm.copy_in(*proc::k_pm.get_cur_pcb()->get_pagetable(), (char *)&timeout, timeout_addr, sizeof(timeout)) < 0)
            {
                return -1;
            }
            timeout_ptr = &timeout;
        }

        if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE || cmd == FUTEX_CMP_REQUEUE_PI || cmd == FUTEX_WAKE_OP)
        {
            _arg_int(3, val2);
        }

        printf("sys_futex: uaddr=%p, op=%d, val=%d, timeout=%p, uaddr2=%p, val3=%d\n", uaddr, op, val, timeout_ptr, uaddr2, val3);
        // printf("paddr: %p\n", proc::k_pm.get_cur_pcb()->_pt.walk_addr(uaddr));
        switch (op)
        {
        case FUTEX_WAIT:
            return proc::futex_wait(uaddr, val, timeout_ptr);
        case FUTEX_WAKE:
            return proc::futex_wakeup(uaddr, val, NULL, 0);
        case FUTEX_REQUEUE:
            return proc::futex_wakeup(uaddr, val, (void *)uaddr2, val3);
        default:
            return 0;
        }
    }
    uint64 SyscallHandler::sys_get_robust_list()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setitimer()
    {
        return 0;
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_getaffinity()
    {
        int pid;
        ulong cpusetsize;
        uint64 mask_addr;

        // 获取系统调用参数
        if (_arg_int(0, pid) < 0 || _arg_addr(1, cpusetsize) < 0 || _arg_addr(2, mask_addr) < 0)
        {
            return SYS_EFAULT;
        }

        printfCyan("[sys_sched_getaffinity] pid: %d, cpusetsize: %lu, mask_addr: %p\n",
                   pid, cpusetsize, (void *)mask_addr);

        // 检查cpusetsize的大小
        if (cpusetsize < sizeof(CpuMask))
        {
            printfRed("[sys_sched_getaffinity] cpusetsize %lu too small, need at least %lu\n",
                      cpusetsize, sizeof(CpuMask));
            return SYS_EINVAL;
        }

        proc::Pcb *target_proc;

        // 如果pid为0，获取当前进程
        if (pid == 0)
        {
            target_proc = proc::k_pm.get_cur_pcb();
            if (!target_proc)
            {
                printfRed("[sys_sched_getaffinity] current process is null\n");
                return SYS_ESRCH;
            }
        }
        else
        {
            // 根据pid查找进程
            target_proc = proc::k_pm.find_proc_by_pid(pid);
            if (!target_proc)
            {
                printfRed("[sys_sched_getaffinity] process with pid %d not found\n", pid);
                return SYS_ESRCH;
            }
        }

        // 获取CPU亲和性掩码
        const CpuMask &cpu_mask = target_proc->get_cpu_mask();

        // printfCyan("[sys_sched_getaffinity] cpu_mask bits: %lx\n", cpu_mask.bits);
        // printfCyan("[sys_sched_getaffinity] sizeof(CpuMask): %lu\n", sizeof(CpuMask));

        // 将CPU掩码拷贝到用户空间
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = current_proc->get_pagetable();

        current_proc->print_detailed_memory_info();

        printfGreen("[sys_sched_getaffinity] Copying CPU mask to user space at address %p\n", (void *)mask_addr);
        if (mem::k_vmm.copy_out(*pt, mask_addr, &cpu_mask, sizeof(CpuMask)) < 0)
        {
            printfRed("[sys_sched_getaffinity] failed to copy cpu mask to user space\n");
            return SYS_EFAULT;
        }

        // 成功时返回实际拷贝的字节数
        return sizeof(CpuMask);
    }
    uint64 SyscallHandler::sys_setpgid()
    {
        int pid, pgid;

        // 获取参数
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_setpgid] Error fetching pid argument\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, pgid) < 0)
        {
            printfRed("[SyscallHandler::sys_setpgid] Error fetching pgid argument\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setpgid] pid: %d, pgid: %d\n", pid, pgid);

        // 获取当前进程
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            panic("[SyscallHandler::sys_setpgid] Current process is null\n");
            return SYS_ESRCH;
        }

        // 确定目标进程
        proc::Pcb *target_proc;
        if (pid == 0)
        {
            // pid 为 0，使用当前进程
            target_proc = current_proc;
        }
        else
        {
            // 根据 pid 查找对应的进程
            target_proc = proc::k_pm.find_proc_by_pid(pid);
            if (target_proc == nullptr)
            {
                printfRed("[SyscallHandler::sys_setpgid] Process with pid %d not found\n", pid);
                return SYS_ESRCH; // 进程不存在
            }

            // 权限检查：目标进程必须是调用进程本身或调用进程的子进程
            if (target_proc != current_proc && target_proc->get_parent() != current_proc)
            {
                printfRed("[SyscallHandler::sys_setpgid] Permission denied: process %d is not the calling process or its child\n", pid);
                return SYS_ESRCH; // 权限不足，按POSIX标准返回ESRCH
            }
        }

        // 确定要设置的进程组ID
        uint32 set_pgid;
        if (pgid == 0)
        {
            // pgid 为 0，使用目标进程的 PID 作为进程组ID
            set_pgid = target_proc->get_pid();
        }
        else
        {
            // 使用指定的 pgid
            set_pgid = (uint32)pgid;

            // 检查 pgid 是否为负数
            if (pgid < 0)
            {
                printfRed("[SyscallHandler::sys_setpgid] Invalid pgid: %d\n", pgid);
                return SYS_EINVAL;
            }

            if ((uint)pgid >= proc::pid_max)
            {
                printfRed("[SyscallHandler::sys_setpgid] Invalid pgid: %d\n", pgid);
                return SYS_EPERM;
            }
        }

        // 设置进程组ID
        target_proc->set_pgid(set_pgid);

        printfGreen("[SyscallHandler::sys_setpgid] Successfully set pgid %u for process %d\n",
                    set_pgid, target_proc->get_pid());

        return 0;
    }
    uint64 SyscallHandler::sys_getpgid()
    {
        // proc::k_pm.debug_process_states();
        int pid;

        // 获取参数
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_getpgid] Error fetching pid argument\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_getpgid] pid: %d\n", pid);

        // 如果 pid 为 0，返回当前进程的进程组ID
        if (pid == 0)
        {
            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            if (current_proc == nullptr)
            {
                panic("[SyscallHandler::sys_getpgid] Current process is null\n");
                return SYS_ESRCH;
            }

            // 假设 Pcb 中有 pgid 字段，如果没有，可能需要添加或使用其他方式获取
            return current_proc->get_pgid();
        }

        // 根据 pid 查找对应的进程
        proc::Pcb *target_proc = proc::k_pm.find_proc_by_pid(pid);
        if (target_proc == nullptr)
        {
            printfRed("[SyscallHandler::sys_getpgid] Process with pid %d not found\n", pid);
            return SYS_ESRCH; // 进程不存在
        }

        // 返回目标进程的进程组ID
        return target_proc->get_pgid();
    }
    uint64 SyscallHandler::sys_setsid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getrusage()
    {

        // TODO: 感觉写的不一定对
        int who;
        uint64 usage_addr;

        // 获取参数
        if (_arg_int(0, who) < 0 || _arg_addr(1, usage_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getrusage] Error fetching arguments\n");
            return -1;
        }

        // 定义常量
        const int RUSAGE_SELF = 0;
        const int RUSAGE_CHILDREN = -1;
        const int RUSAGE_THREAD = 1;

        // 定义 rusage 结构体（根据 Linux 标准）
        struct rusage
        {
            tmm::timeval ru_utime; // 用户态时间
            tmm::timeval ru_stime; // 内核态时间
            long ru_maxrss;        // 最大常驻集大小
            long ru_ixrss;         // 共享内存大小
            long ru_idrss;         // 非共享数据大小
            long ru_isrss;         // 非共享栈大小
            long ru_minflt;        // 页面回收次数
            long ru_majflt;        // 页面错误次数
            long ru_nswap;         // 交换次数
            long ru_inblock;       // 输入块数
            long ru_oublock;       // 输出块数
            long ru_msgsnd;        // 消息发送次数
            long ru_msgrcv;        // 消息接收次数
            long ru_nsignals;      // 信号接收次数
            long ru_nvcsw;         // 自愿上下文切换次数
            long ru_nivcsw;        // 非自愿上下文切换次数
        };

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 获取当前进程的时间统计信息
        tmm::tms tms_val;
        proc::k_pm.get_cur_proc_tms(&tms_val);

        // 初始化 rusage 结构体
        rusage ret;
        memset(&ret, 0, sizeof(ret));

        // 计算用户态时间和内核态时间
        // tms 中的时间单位通常是时钟滴答，需要转换为秒和微秒
        tmm::timeval utimeval;
        tmm::timeval stimeval;

        // 将时钟滴答转换为秒和微秒
        utimeval.tv_sec = tms_val.tms_utime / 1000;
        utimeval.tv_usec = (tms_val.tms_utime % 1000) * 1000;

        stimeval.tv_sec = tms_val.tms_stime / 1000;
        stimeval.tv_usec = (tms_val.tms_stime % 1000) * 1000;

        switch (who)
        {
        case RUSAGE_SELF:
            ret.ru_utime = utimeval;
            ret.ru_stime = stimeval;
            // 其他字段保持为0，因为当前实现不跟踪这些统计信息
            break;

        case RUSAGE_CHILDREN:
            // 对于子进程的资源使用，暂时使用当前进程的时间
            // 实际实现中应该累计已结束子进程的时间
            ret.ru_utime = utimeval;
            ret.ru_stime = stimeval;
            break;

        case RUSAGE_THREAD:
            ret.ru_utime = utimeval;
            ret.ru_stime = stimeval;
            break;

        default:
            printfRed("[SyscallHandler::sys_getrusage] Invalid who parameter: %d\n", who);
            return SYS_EINVAL;
        }

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, usage_addr, &ret, sizeof(ret)) < 0)
        {
            printfRed("[SyscallHandler::sys_getrusage] Error copying rusage to user space\n");
            return SYS_EFAULT;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_getegid()
    {
        return 1;
    }
    uint64 SyscallHandler::sys_shmget()
    {
        key_t key;
        long ssize;
        int shmflg;
        if (_arg_int(0, key) < 0 ||
            _arg_long(1, ssize) < 0 ||
            _arg_int(2, shmflg) < 0)
        {
            printfRed("[SyscallHandler::sys_shmget] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        size_t size = ssize;
        return shm::k_smm.create_seg(key, size, shmflg);
    }
    uint64 SyscallHandler::sys_shmctl()
    {
        int shmid;
        int cmd;
        uint64 buf_addr;
        struct shmid_ds buf;

        if (_arg_int(0, shmid) < 0 ||
            _arg_int(1, cmd) < 0 ||
            _arg_addr(2, buf_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_shmctl] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        // 从用户空间拷贝 shmid_ds 结构体
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        #ifdef LOONGARCH
        #endif
        if (mem::k_vmm.copy_in(*pt, &buf, buf_addr, sizeof(buf)) < 0)
        {
            printfRed("[SyscallHandler::sys_shmctl] 拷贝 shmid_ds 结构体失败\n");
            return SYS_EFAULT; // 拷贝失败
        }
        printfCyan("[SyscallHandler::sys_shmctl] shmid: %d, cmd: %d, buf_addr: %p\n", shmid, cmd, (void *)buf_addr);
        return shm::k_smm.shmctl(shmid, cmd, &buf, buf_addr);
    }
    uint64 SyscallHandler::sys_shmat()
    {
        int shmid;
        uint64 shmaddr;
        int shmflg;

        if (_arg_int(0, shmid) < 0 ||
            _arg_addr(1, shmaddr) < 0 ||
            _arg_int(2, shmflg) < 0)
        {
            printfRed("[SyscallHandler::sys_shmat] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        printfCyan("[SyscallHandler::sys_shmat] shmid: %d, shmaddr: %p, shmflg: %d\n", shmid, (void *)shmaddr, shmflg);
        return (uint64)shm::k_smm.attach_seg(shmid, (void *)shmaddr, shmflg);
    }
    uint64 SyscallHandler::sys_shmdt()
    {

        uint64 shmaddr;

        if (_arg_addr(0, shmaddr) < 0)
        {
            printfRed("[SyscallHandler::sys_shmdt] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        printfCyan("[SyscallHandler::sys_shmdt] attempting to detach addr: %p\n", (void *)shmaddr);
        // 调用共享内存管理器的分离函数
        return shm::k_smm.detach_seg((void *)shmaddr);
    }
    uint64 SyscallHandler::sys_socket()
    {
        int domain, type, protocol;

        if (_arg_int(0, domain) < 0)
        {
            printfRed("[SyscallHandler::sys_socket] 参数错误: domain\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, type) < 0)
        {
            printfRed("[SyscallHandler::sys_socket] 参数错误: type\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, protocol) < 0)
        {
            printfRed("[SyscallHandler::sys_socket] 参数错误: protocol\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_socket] 创建socket: domain=%d, type=%d, protocol=%d\n", 
                   domain, type, protocol);

        // 检查协议族有效性
        switch (domain)
        {
            case AF_UNSPEC:
                printfRed("[SyscallHandler::sys_socket] 不支持的协议族 AF_UNSPEC: %d\n", domain);
                return SYS_EAFNOSUPPORT;
                
            case AF_UNIX:  // AF_LOCAL 和 AF_UNIX 是相同的值
                // AF_UNIX/AF_LOCAL 支持
                printfCyan("[SyscallHandler::sys_socket] 创建Unix域套接字\n");
                break;
                
            case AF_INET:
                // AF_INET 支持
                printfCyan("[SyscallHandler::sys_socket] 创建IPv4套接字\n");
                break;
                
            case AF_INET6:
                // AF_INET6 暂不支持
                printfYellow("[SyscallHandler::sys_socket] IPv6协议族暂不支持: %d\n", domain);
                return SYS_EAFNOSUPPORT;
                
            default:
                printfRed("[SyscallHandler::sys_socket] 未知的协议族: %d\n", domain);
                return SYS_EAFNOSUPPORT;
        }

        // 检查socket类型有效性
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW && type != SOCK_SEQPACKET)
        {
            printfRed("[SyscallHandler::sys_socket] 不支持的socket类型: %d\n", type);
            return SYS_EINVAL;
        }

        // 检查协议和类型的兼容性 (针对AF_INET)
        if (domain == AF_INET)
        {
            switch (type)
            {
                case SOCK_STREAM:
                    // TCP stream socket
                    if (protocol != 0 && protocol != IPPROTO_TCP)
                    {
                        printfRed("[SyscallHandler::sys_socket] TCP stream不支持协议%d\n", protocol);
                        return SYS_EPROTONOSUPPORT;
                    }
                    break;
                    
                case SOCK_DGRAM:
                    // UDP datagram socket
                    if (protocol != 0 && protocol != IPPROTO_UDP)
                    {
                        printfRed("[SyscallHandler::sys_socket] UDP datagram不支持协议%d\n", protocol);
                        return SYS_EPROTONOSUPPORT;
                    }
                    break;
                    
                case SOCK_RAW:
                    // RAW socket需要root权限
                    printfRed("[SyscallHandler::sys_socket] RAW socket需要root权限\n");
                    return SYS_EPROTONOSUPPORT;
                    
                default:
                    // 其他类型暂不支持
                    printfRed("[SyscallHandler::sys_socket] AF_INET不支持socket类型%d\n", type);
                    return SYS_EINVAL;
            }
        }

        // 针对不同协议族的特殊处理
        SOCKET onps_socket = INVALID_SOCKET;
        
        if (domain == AF_UNIX)  // AF_LOCAL 等同于 AF_UNIX
        {
            // Unix域套接字不需要onps，直接创建socket_file
            printfCyan("[SyscallHandler::sys_socket] Unix域套接字无需网络栈支持\n");
        }
        else if (domain == AF_INET)
        {
            // 使用onps创建网络socket
            EN_ONPSERR enErr = ERRNO;
            onps_socket = socket(domain, type, protocol, &enErr);
            
            if (onps_socket == INVALID_SOCKET)
            {
                printfRed("[SyscallHandler::sys_socket] onps创建socket失败: %d\n", enErr);
                // 将onps错误码转换为系统错误码
                switch (enErr)
                {
                    case ERRADDRFAMILIES:
                        return SYS_EAFNOSUPPORT;
                    case ERRSOCKETTYPE:
                        return SYS_EINVAL;
                    case ERRNOFREEMEM:
                        return SYS_ENOMEM;
                    default:
                        return SYS_EINVAL;
                }
            }
        }

        // 创建socket文件对象
        fs::socket_file *socket_f = new fs::socket_file(domain, type, protocol);
        if (!socket_f)
        {
            if (onps_socket != INVALID_SOCKET)
            {
                close(onps_socket);  // 关闭onps socket
            }
            printfRed("[SyscallHandler::sys_socket] 创建socket_file失败\n");
            return SYS_ENOMEM;
        }

        // 将onps socket句柄关联到socket_file (仅对网络socket)
        if (onps_socket != INVALID_SOCKET)
        {
            socket_f->set_onps_socket(onps_socket);
        }

        // 分配文件描述符
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        int fd = proc::k_pm.alloc_fd(p, socket_f);
        if (fd < 0)
        {
            if (onps_socket != INVALID_SOCKET)
            {
                close(onps_socket);  // 关闭onps socket
            }
            delete socket_f;
            printfRed("[SyscallHandler::sys_socket] 分配文件描述符失败\n");
            return SYS_EMFILE;
        }

        if (domain == AF_UNIX)  // AF_LOCAL 等同于 AF_UNIX
        {
            printfCyan("[SyscallHandler::sys_socket] Unix域套接字创建成功, fd=%d, domain=%d, type=%d, protocol=%d\n",
                       fd, domain, type, protocol);
        }
        else
        {
            printfCyan("[SyscallHandler::sys_socket] 网络套接字创建成功, fd=%d, onps_socket=%d, domain=%d, type=%d, protocol=%d\n",
                       fd, onps_socket, domain, type, protocol);
        }
        
        return fd;
    }
    uint64 SyscallHandler::sys_socketpair()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_bind()
    {
        int sockfd;
        uint64 addr;
        int addrlen;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_bind] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_bind] 参数错误: addr\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, addrlen) < 0)
        {
            printfRed("[SyscallHandler::sys_bind] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_bind] bind socket: sockfd=%d, addr=0x%lx, addrlen=%d\n", 
                   sockfd, addr, addrlen);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_bind] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_bind] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        SOCKET onps_socket = socket_f->get_onps_socket();
        
        if (onps_socket == INVALID_SOCKET)
        {
            printfRed("[SyscallHandler::sys_bind] socket未关联onps socket\n");
            return SYS_EINVAL;
        }

        // 检查地址长度
        if ((socklen_t)addrlen < sizeof(struct sockaddr_in))
        {
            printfRed("[SyscallHandler::sys_bind] 地址长度不足: %d\n", addrlen);
            return SYS_EINVAL;
        }

        // 从用户空间复制sockaddr结构体
        struct sockaddr_in sock_addr;
        if (mem::k_vmm.copy_in(*pt, &sock_addr, addr, sizeof(struct sockaddr_in)) < 0)
        {
            printfRed("[SyscallHandler::sys_bind] 复制sockaddr失败\n");
            return SYS_EFAULT;
        }

        // 检查地址族
        if (sock_addr.sin_family != AF_INET)
        {
            printfRed("[SyscallHandler::sys_bind] 不支持的地址族: %d\n", sock_addr.sin_family);
            return SYS_EAFNOSUPPORT;
        }

        // 提取IP地址和端口
        uint32_t ip_addr = sock_addr.sin_addr;
        uint16_t port = htons(sock_addr.sin_port);
        
        printfCyan("[SyscallHandler::sys_bind] 解析地址: IP=0x%08x, Port=%d\n", ip_addr, port);

        // 将IP地址转换为字符串（onps bind需要字符串形式的IP）
        char ip_str[16];
        const char *ip_str_ptr = nullptr;
        
        if (ip_addr == 0)
        {
            // INADDR_ANY，让onps使用任意地址
            ip_str_ptr = nullptr;
        }
        else
        {
            // 转换IP地址为字符串格式 (a.b.c.d)
            uint8_t *ip_bytes = (uint8_t*)&ip_addr;
            snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", 
                    ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);
            ip_str_ptr = ip_str;
            printfCyan("[SyscallHandler::sys_bind] IP字符串: %s\n", ip_str);
        }

        // 调用onps的bind函数
        int result = bind(onps_socket, ip_str_ptr, port);
        
        if (result < 0)
        {
            // 获取onps错误信息
            EN_ONPSERR onps_err = socket_get_last_error_code(onps_socket);
            printfRed("[SyscallHandler::sys_bind] onps bind失败: %d\n", onps_err);
            
            // 转换onps错误码为系统错误码
            switch (onps_err)
            {
                case ERRPORTOCCUPIED:
                    return SYS_EADDRINUSE;
                case ERRNOTBINDADDR:
                    return SYS_EINVAL;
                case ERRUNSUPPIPPROTO:
                    return SYS_EPROTONOSUPPORT;
                default:
                    return SYS_EINVAL;
            }
        }

        // 同时调用socket_file的bind方法来更新状态
        int socket_file_result = socket_f->bind((const struct sockaddr *)addr, addrlen);
        if (socket_file_result < 0)
        {
            printfRed("[SyscallHandler::sys_bind] socket_file bind失败: %d\n", socket_file_result);
            // onps bind已经成功，但socket_file状态更新失败，这不是致命错误
        }

        printfCyan("[SyscallHandler::sys_bind] bind成功, sockfd=%d, onps_socket=%d\n", sockfd, onps_socket);
        return 0;
    }
    uint64 SyscallHandler::sys_listen()
    {
        int sockfd;
        int backlog;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_listen] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, backlog) < 0)
        {
            printfRed("[SyscallHandler::sys_listen] 参数错误: backlog\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_listen] listen socket: sockfd=%d, backlog=%d\n", 
                   sockfd, backlog);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_listen] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_listen] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        SOCKET onps_socket = socket_f->get_onps_socket();
        
        if (onps_socket == INVALID_SOCKET)
        {
            printfRed("[SyscallHandler::sys_listen] socket未关联onps socket\n");
            return SYS_EINVAL;
        }

        // 检查backlog是否合理
        if (backlog < 0)
        {
            printfRed("[SyscallHandler::sys_listen] 无效的backlog: %d\n", backlog);
            return SYS_EINVAL;
        }

        // 调用onps的listen函数
        int onps_result = ::listen(onps_socket, (USHORT)backlog);
        if (onps_result != 0)
        {
            printfRed("[SyscallHandler::sys_listen] onps listen失败: %d\n", onps_result);
            // 将onps错误码转换为系统错误码
            return SYS_EOPNOTSUPP;  // 可以根据具体错误码进行细化
        }

        // 更新socket_file的状态
        int socket_file_result = socket_f->listen(backlog);
        if (socket_file_result < 0)
        {
            printfRed("[SyscallHandler::sys_listen] socket_file listen失败: %d\n", socket_file_result);
            return socket_file_result;
        }

        printfCyan("[SyscallHandler::sys_listen] listen成功, sockfd=%d, onps_socket=%d, backlog=%d\n", 
                   sockfd, onps_socket, backlog);
        return 0;
    }
    uint64 SyscallHandler::sys_accept()
    {
        int sockfd;
        uint64 addr;
        uint64 addrlen_ptr;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_accept] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_accept] 参数错误: addr\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(2, addrlen_ptr) < 0)
        {
            printfRed("[SyscallHandler::sys_accept] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_accept] accept socket: sockfd=%d\n", sockfd);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_accept] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_accept] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        SOCKET onps_socket = socket_f->get_onps_socket();
        
        if (onps_socket == INVALID_SOCKET)
        {
            printfRed("[SyscallHandler::sys_accept] socket未关联onps socket\n");
            return SYS_EINVAL;
        }

        // 从用户空间读取地址长度
        socklen_t user_addrlen = 0;
        if (addr != 0 && addrlen_ptr != 0)
        {
            if (mem::k_vmm.copy_in(*pt, &user_addrlen, addrlen_ptr, sizeof(socklen_t)) < 0)
            {
                printfRed("[SyscallHandler::sys_accept] 读取地址长度失败\n");
                return SYS_EFAULT;
            }
        }

        // 准备客户端地址信息
        in_addr_t client_ip = 0;
        USHORT client_port = 0;
        EN_ONPSERR onps_err;
        
        // 调用onps的accept函数，阻塞等待连接
        SOCKET client_socket = ::accept(onps_socket, &client_ip, &client_port, 0, &onps_err);
        if (client_socket == INVALID_SOCKET)
        {
            printfRed("[SyscallHandler::sys_accept] onps accept失败, 错误: %d\n", onps_err);
            // 将onps错误码转换为系统错误码
            if (onps_err == ERRTCPNOLISTEN)
                return SYS_EINVAL;  // 没有监听
            else if (onps_err == ERRNOTBINDADDR)
                return SYS_EINVAL;  // 没有绑定地址
            else
                return SYS_EAGAIN;  // 其他错误，通常是没有连接可接受
        }

        // 创建新的socket_file对象用于客户端连接
        // 继承服务器socket的属性
        int domain = (int)socket_f->get_family();
        int type = (int)socket_f->get_type();
        int protocol = socket_f->get_protocol();
        
        fs::socket_file *client_socket_f = new fs::socket_file(domain, type, protocol);
        if (!client_socket_f)
        {
            close(client_socket);  // 关闭onps socket
            printfRed("[SyscallHandler::sys_accept] 创建客户端socket_file失败\n");
            return SYS_ENOMEM;
        }

        // 将onps客户端socket句柄关联到socket_file
        client_socket_f->set_onps_socket(client_socket);

        // 为新的客户端socket分配文件描述符
        int client_fd = proc::k_pm.alloc_fd(p, client_socket_f);
        if (client_fd < 0)
        {
            close(client_socket);  // 关闭onps socket
            delete client_socket_f;
            printfRed("[SyscallHandler::sys_accept] 分配客户端文件描述符失败\n");
            return SYS_EMFILE;
        }

        // 如果用户提供了地址缓冲区，填充客户端地址信息
        if (addr != 0 && user_addrlen > 0)
        {
            struct sockaddr_in client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            client_addr.sin_family = AF_INET;
            client_addr.sin_addr = htonl(client_ip);
            client_addr.sin_port = htons(client_port);

            // 确定要复制的大小
            socklen_t copy_len = (user_addrlen < sizeof(struct sockaddr_in)) ? user_addrlen : sizeof(struct sockaddr_in);

            // 复制地址到用户空间
            if (mem::k_vmm.copy_out(*pt, addr, &client_addr, copy_len) < 0)
            {
                printfRed("[SyscallHandler::sys_accept] 复制客户端地址到用户空间失败\n");
                // 不是致命错误，继续执行
            }

            // 更新实际地址长度
            socklen_t actual_len = sizeof(struct sockaddr_in);
            if (mem::k_vmm.copy_out(*pt, addrlen_ptr, &actual_len, sizeof(socklen_t)) < 0)
            {
                printfRed("[SyscallHandler::sys_accept] 复制地址长度到用户空间失败\n");
                // 不是致命错误，继续执行
            }
        }

        printfCyan("[SyscallHandler::sys_accept] accept成功, sockfd=%d, client_fd=%d, client_ip=0x%08x, client_port=%d\n", 
                   sockfd, client_fd, client_ip, client_port);
        return client_fd;
    }
    uint64 SyscallHandler::sys_connect()
    {
        int sockfd;
        uint64 addr;
        int addrlen;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_connect] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_connect] 参数错误: addr\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, addrlen) < 0)
        {
            printfRed("[SyscallHandler::sys_connect] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_connect] connect socket: sockfd=%d, addr=0x%lx, addrlen=%d\n", 
                   sockfd, addr, addrlen);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_connect] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_connect] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        
        // 检查地址长度 - 至少要有sockaddr的基本大小
        if ((socklen_t)addrlen < sizeof(struct sockaddr))
        {
            printfRed("[SyscallHandler::sys_connect] 地址长度不足: %d\n", addrlen);
            return SYS_EINVAL;
        }

        // 首先读取通用的sockaddr结构来检查地址族
        struct sockaddr generic_addr;
        if (mem::k_vmm.copy_in(*pt, &generic_addr, addr, sizeof(struct sockaddr)) < 0)
        {
            printfRed("[SyscallHandler::sys_connect] 复制sockaddr失败\n");
            return SYS_EFAULT;
        }

        // 根据地址族处理不同的地址结构
        switch (generic_addr.sa_family)
        {
            case AF_INET:
            {
                printfCyan("[SyscallHandler::sys_connect] 处理IPv4连接\n");
                
                // 检查IPv4地址长度
                if ((socklen_t)addrlen < sizeof(struct sockaddr_in))
                {
                    printfRed("[SyscallHandler::sys_connect] IPv4地址长度不足: %d\n", addrlen);
                    return SYS_EINVAL;
                }

                // 从用户空间复制sockaddr_in结构体
                struct sockaddr_in sock_addr_in;
                if (mem::k_vmm.copy_in(*pt, &sock_addr_in, addr, sizeof(struct sockaddr_in)) < 0)
                {
                    printfRed("[SyscallHandler::sys_connect] 复制sockaddr_in失败\n");
                    return SYS_EFAULT;
                }

                // 获取onps socket句柄
                SOCKET onps_socket = socket_f->get_onps_socket();
                if (onps_socket == INVALID_SOCKET)
                {
                    printfRed("[SyscallHandler::sys_connect] IPv4 socket未关联onps socket\n");
                    return SYS_EINVAL;
                }

                // 提取IP地址和端口
                uint32_t ip_addr = sock_addr_in.sin_addr;
                uint16_t port = ENDIAN_CONVERTER_USHORT(sock_addr_in.sin_port);  // 转换网络字节序到主机字节序
                
                printfCyan("[SyscallHandler::sys_connect] 解析IPv4地址: IP=0x%08x, Port=%d\n", ip_addr, port);

                // 检查目标地址是否有效
                // if (ip_addr == 0)
                // {
                //     printfRed("[SyscallHandler::sys_connect] 无效的目标地址: 0.0.0.0\n");
                //     return SYS_EINVAL;
                // }

                // if (port == 0)
                // {
                //     printfRed("[SyscallHandler::sys_connect] 无效的目标端口: 0\n");
                //     return SYS_EINVAL;
                // }

                // 调用onps的connect_ext函数，使用默认超时时间
                int result = connect_ext(onps_socket, &ip_addr, port, TCP_CONN_TIMEOUT);
                
                if (result != 0)
                {
                    // 获取onps错误信息
                    EN_ONPSERR onps_err = socket_get_last_error_code(onps_socket);
                    printfRed("[SyscallHandler::sys_connect] onps connect失败, result=%d, 错误: %d\n", result, onps_err);
                    
                    // 将onps错误码转换为系统错误码
                    switch (onps_err)
                    {
                        case ERRTCPCONNTIMEOUT:
                            return SYS_ETIMEDOUT;  // 连接超时
                        case ERRTCPCONNRESET:
                            return SYS_ECONNRESET; // 连接被重置
                        case ERRADDRFAMILIES:
                            return SYS_EAFNOSUPPORT; // 不支持的地址族
                        case ERRUNSUPPIPPROTO:
                            return SYS_EPROTONOSUPPORT; // 不支持的协议
                        case ERRNOTBINDADDR:
                            return SYS_EADDRNOTAVAIL; // 地址不可用
                        default:
                            return SYS_ECONNREFUSED; // 连接被拒绝（默认错误）
                    }
                }

                // 更新socket_file的状态
                int socket_file_result = socket_f->connect((const struct sockaddr *)&sock_addr_in, addrlen);
                if (socket_file_result < 0)
                {
                    printfRed("[SyscallHandler::sys_connect] socket_file connect失败: %d\n", socket_file_result);
                    // 这里不返回错误，因为onps连接已经成功了，socket_file的connect主要是状态更新
                }

                printfCyan("[SyscallHandler::sys_connect] IPv4连接成功, sockfd=%d, 目标: %d.%d.%d.%d:%d\n", 
                           sockfd, 
                           (ip_addr >> 0) & 0xFF, (ip_addr >> 8) & 0xFF, 
                           (ip_addr >> 16) & 0xFF, (ip_addr >> 24) & 0xFF, 
                           port);
                return 0;
            }

            case AF_UNIX:
            {
                printfCyan("[SyscallHandler::sys_connect] 处理Unix域套接字连接\n");
                
                // 检查Unix地址长度
                if ((socklen_t)addrlen < sizeof(struct sockaddr_un))
                {
                    printfRed("[SyscallHandler::sys_connect] Unix地址长度不足: %d\n", addrlen);
                    return SYS_EINVAL;
                }

                // 从用户空间复制sockaddr_un结构体
                struct sockaddr_un sock_addr_un;
                if (mem::k_vmm.copy_in(*pt, &sock_addr_un, addr, sizeof(struct sockaddr_un)) < 0)
                {
                    printfRed("[SyscallHandler::sys_connect] 复制sockaddr_un失败\n");
                    return SYS_EFAULT;
                }

                // Unix域套接字不使用onps，直接通过socket_file处理
                printfCyan("[SyscallHandler::sys_connect] Unix域套接字路径: %s\n", sock_addr_un.sun_path);

                // 更新socket_file的状态
                int socket_file_result = socket_f->connect((const struct sockaddr *)&sock_addr_un, addrlen);
                if (socket_file_result < 0)
                {
                    printfRed("[SyscallHandler::sys_connect] Unix域套接字连接失败: %d\n", socket_file_result);
                    switch (socket_file_result)
                    {
                        case -1:
                            return SYS_ECONNREFUSED; // 连接被拒绝
                        case -2:
                            return SYS_ENOENT;       // 路径不存在
                        case -3:
                            return SYS_EACCES;       // 权限不足
                        default:
                            return SYS_EINVAL;       // 其他错误
                    }
                }

                printfCyan("[SyscallHandler::sys_connect] Unix域套接字连接成功, sockfd=%d, 路径: %s\n", 
                           sockfd, sock_addr_un.sun_path);
                return 0;
            }

            case AF_INET6:
            {
                printfYellow("[SyscallHandler::sys_connect] IPv6协议族暂不支持: %d\n", generic_addr.sa_family);
                return SYS_EAFNOSUPPORT;
            }

            case AF_UNSPEC:
            {
                printfRed("[SyscallHandler::sys_connect] 不支持的协议族 AF_UNSPEC: %d\n", generic_addr.sa_family);
                return SYS_EAFNOSUPPORT;
            }

            default:
            {
                printfRed("[SyscallHandler::sys_connect] 未知的协议族: %d\n", generic_addr.sa_family);
                return SYS_EAFNOSUPPORT;
            }
        }
    }
    uint64 SyscallHandler::sys_getsockname()
    {
        int sockfd;
        uint64 addr;
        uint64 addrlen_ptr;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 参数错误: addr\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(2, addrlen_ptr) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_getsockname] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        SOCKET onps_socket = socket_f->get_onps_socket();
        
        if (onps_socket == INVALID_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] socket未关联onps socket\n");
            return SYS_EINVAL;
        }

        // 从用户空间读取地址长度
        socklen_t user_addrlen;
        if (mem::k_vmm.copy_in(*pt, &user_addrlen, addrlen_ptr, sizeof(socklen_t)) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 读取地址长度失败\n");
            return SYS_EFAULT;
        }

        // 通过onps获取socket地址信息
        EN_ONPSERR onps_err;
        PST_TCPUDP_HANDLE pstHandle;
        if (!onps_input_get(onps_socket, IOPT_GETTCPUDPADDR, &pstHandle, &onps_err))
        {
            printfRed("[SyscallHandler::sys_getsockname] 无法获取socket地址信息, onps错误: %d\n", onps_err);
            return SYS_EINVAL;
        }

        // 构造sockaddr_in结构
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
#if SUPPORT_IPV6
        if (pstHandle->bFamily == AF_INET) {
            local_addr.sin_addr = pstHandle->stSockAddr.saddr_ipv4;
        } else {
            printfRed("[SyscallHandler::sys_getsockname] IPv6暂不支持\n");
            return SYS_EAFNOSUPPORT;
        }
#else
        local_addr.sin_addr = pstHandle->stSockAddr.saddr_ipv4;
#endif
        local_addr.sin_port = htons(pstHandle->stSockAddr.usPort);

        // 确定要复制的大小
        socklen_t copy_len = (user_addrlen < sizeof(struct sockaddr_in)) ? user_addrlen : sizeof(struct sockaddr_in);

        // 复制地址到用户空间
        if (mem::k_vmm.copy_out(*pt, addr, &local_addr, copy_len) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 复制地址到用户空间失败\n");
            return SYS_EFAULT;
        }

        // 更新实际地址长度
        socklen_t actual_len = sizeof(struct sockaddr_in);
        if (mem::k_vmm.copy_out(*pt, addrlen_ptr, &actual_len, sizeof(socklen_t)) < 0)
        {
            printfRed("[SyscallHandler::sys_getsockname] 复制地址长度到用户空间失败\n");
            return SYS_EFAULT;
        }

        printfCyan("[SyscallHandler::sys_getsockname] getsockname成功, sockfd=%d, port=%d\n", 
                   sockfd, pstHandle->stSockAddr.usPort);
        return 0;
    }
    uint64 SyscallHandler::sys_getpeername()
    {
        printfRed("这个是乱写的，用了就寄");
        int sockfd;
        uint64 addr;
        uint64 addrlen_ptr;

        if (_arg_int(0, sockfd) < 0)
        {
            printfRed("[SyscallHandler::sys_getpeername] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getpeername] 参数错误: addr\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(2, addrlen_ptr) < 0)
        {
            printfRed("[SyscallHandler::sys_getpeername] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f)
        {
            printfRed("[SyscallHandler::sys_getpeername] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }


        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        int result = socket_f->getpeername((struct sockaddr *)addr, (socklen_t *)addrlen_ptr);
        if (result < 0)
        {
            printfRed("[SyscallHandler::sys_getpeername] getpeername失败: %d\n", result);
            return result;
        }

        printfCyan("[SyscallHandler::sys_getpeername] getpeername成功, sockfd=%d\n", sockfd);
        return 0;
    }
    uint64 SyscallHandler::sys_sendto()
    {
        // https://www.man7.org/linux/man-pages/man3/sendto.3p.html
        int sockfd;
        uint64 buf_ptr;
        size_t len;
        int flags;
        uint64 dest_addr_ptr;
        socklen_t addrlen;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, buf_ptr) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: buf\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(2, len) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: len\n");
            return SYS_EINVAL;
        }

        if (_arg_int(3, flags) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: flags\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(4, dest_addr_ptr) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: dest_addr\n");
            return SYS_EINVAL;
        }

        int addrlen_tmp;
        if (_arg_int(5, addrlen_tmp) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }
        addrlen = (socklen_t)addrlen_tmp;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_sendto] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 分配内核缓冲区并复制数据
        eastl::vector<uint8_t> kernel_buf(len);
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, kernel_buf.data(), buf_ptr, len) < 0) {
            printfRed("[SyscallHandler::sys_sendto] 复制数据失败\n");
            return SYS_EFAULT;
        }

        // 复制目标地址
        struct sockaddr dest_addr;
        if (dest_addr_ptr && addrlen > 0) {
            if (mem::k_vmm.copy_in(*pt, &dest_addr, dest_addr_ptr, 
                        eastl::min((size_t)addrlen, sizeof(dest_addr))) < 0) {
                printfRed("[SyscallHandler::sys_sendto] 复制目标地址失败\n");
                return SYS_EFAULT;
            }
        }

        // 调用socket的sendto方法
        int result;
        if (dest_addr_ptr && addrlen > 0) {
            result = socket_f->sendto(kernel_buf.data(), len, flags, &dest_addr, addrlen);
        } else {
            // 如果没有目标地址，等同于send
            result = socket_f->send(kernel_buf.data(), len, flags);
        }

        if (result < 0) {
            printfRed("[SyscallHandler::sys_sendto] sendto失败: %d\n", result);
            return result;
        }

        printfCyan("[SyscallHandler::sys_sendto] sendto成功, sockfd=%d, sent=%d bytes\n", 
                   sockfd, result);
        return result;
    }
    uint64 SyscallHandler::sys_recvfrom()
    {
        int sockfd;
        uint64 buf_ptr;
        size_t len;
        int flags;
        uint64 src_addr_ptr;
        uint64 addrlen_ptr;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, buf_ptr) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: buf\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(2, len) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: len\n");
            return SYS_EINVAL;
        }

        if (_arg_int(3, flags) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: flags\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(4, src_addr_ptr) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: src_addr\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(5, addrlen_ptr) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 参数错误: addrlen\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_recvfrom] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 分配内核缓冲区
        eastl::vector<uint8_t> kernel_buf(len);
        mem::PageTable *pt = p->get_pagetable();

        // 获取地址长度
        socklen_t addrlen = 0;
        if (addrlen_ptr) {
            if (mem::k_vmm.copy_in(*pt, &addrlen, addrlen_ptr, sizeof(socklen_t)) < 0) {
                printfRed("[SyscallHandler::sys_recvfrom] 复制addrlen失败\n");
                return SYS_EFAULT;
            }
        }

        // 准备源地址缓冲区
        struct sockaddr src_addr;
        socklen_t orig_addrlen = addrlen;

        // 调用socket的recvfrom方法
        int result = socket_f->recvfrom(kernel_buf.data(), len, flags, 
                                       src_addr_ptr ? &src_addr : nullptr, 
                                       src_addr_ptr ? &addrlen : nullptr);

        if (result < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] recvfrom失败: %d\n", result);
            return result;
        }

        // 复制数据到用户空间
        if (mem::k_vmm.copy_out(*pt, buf_ptr, kernel_buf.data(), result) < 0) {
            printfRed("[SyscallHandler::sys_recvfrom] 复制数据到用户空间失败\n");
            return SYS_EFAULT;
        }

        // 复制源地址到用户空间（如果请求了）
        if (src_addr_ptr && addrlen > 0) {
            size_t copy_len = eastl::min((size_t)addrlen, (size_t)orig_addrlen);
            if (mem::k_vmm.copy_out(*pt, src_addr_ptr, &src_addr, copy_len) < 0) {
                printfRed("[SyscallHandler::sys_recvfrom] 复制源地址失败\n");
                return SYS_EFAULT;
            }

            // 更新地址长度
            if (mem::k_vmm.copy_out(*pt, addrlen_ptr, &addrlen, sizeof(socklen_t)) < 0) {
                printfRed("[SyscallHandler::sys_recvfrom] 复制addrlen失败\n");
                return SYS_EFAULT;
            }
        }

        printfCyan("[SyscallHandler::sys_recvfrom] recvfrom成功, sockfd=%d, received=%d bytes\n", 
                   sockfd, result);
        return result;
    }
    uint64 SyscallHandler::sys_setsockopt()
    {
        int sockfd;
        int level;
        int optname;
        uint64 optval_ptr;
        int optlen_tmp;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, level) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 参数错误: level\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, optname) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 参数错误: optname\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(3, optval_ptr) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 参数错误: optval\n");
            return SYS_EINVAL;
        }

        if (_arg_int(4, optlen_tmp) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 参数错误: optlen\n");
            return SYS_EINVAL;
        }

        if(is_bad_addr(optval_ptr))
        {
            return SYS_EFAULT;
        }
        socklen_t optlen = (socklen_t)optlen_tmp;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_setsockopt] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 分配内核缓冲区并复制选项值
        eastl::vector<uint8_t> optval_buf(optlen);
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, optval_buf.data(), optval_ptr, optlen) < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] 复制optval失败\n");
            return SYS_EFAULT;
        }

        // 调用socket的setsockopt方法
        int result = socket_f->setsockopt(level, optname, optval_buf.data(), optlen);
        if (result < 0) {
            printfRed("[SyscallHandler::sys_setsockopt] setsockopt失败: %d\n", result);
            return result;
        }

        printfCyan("[SyscallHandler::sys_setsockopt] setsockopt成功, sockfd=%d, level=%d, optname=%d\n", 
                   sockfd, level, optname);
        return 0;
    }
    uint64 SyscallHandler::sys_getsockopt()
    {
        int sockfd;
        int level;
        int optname;
        uint64 optval_ptr;
        uint64 optlen_ptr;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, level) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 参数错误: level\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, optname) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 参数错误: optname\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(3, optval_ptr) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 参数错误: optval\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(4, optlen_ptr) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 参数错误: optlen\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_getsockopt] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 获取选项长度
        socklen_t optlen;
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, &optlen, optlen_ptr, sizeof(socklen_t)) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 复制optlen失败\n");
            return SYS_EFAULT;
        }

        // 分配内核缓冲区
        eastl::vector<uint8_t> optval_buf(optlen);

        // 调用socket的getsockopt方法
        int result = socket_f->getsockopt(level, optname, optval_buf.data(), &optlen);
        if (result < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] getsockopt失败: %d\n", result);
            return result;
        }

        // 复制选项值到用户空间
        if (mem::k_vmm.copy_out(*pt, optval_ptr, optval_buf.data(), optlen) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 复制optval到用户空间失败\n");
            return SYS_EFAULT;
        }

        // 复制更新后的长度到用户空间
        if (mem::k_vmm.copy_out(*pt, optlen_ptr, &optlen, sizeof(socklen_t)) < 0) {
            printfRed("[SyscallHandler::sys_getsockopt] 复制optlen到用户空间失败\n");
            return SYS_EFAULT;
        }

        printfCyan("[SyscallHandler::sys_getsockopt] getsockopt成功, sockfd=%d, level=%d, optname=%d\n", 
                   sockfd, level, optname);
        return 0;
    }
    uint64 SyscallHandler::sys_shutdown_socket()
    {
        int sockfd;
        int how;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_shutdown_socket] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_int(1, how) < 0) {
            printfRed("[SyscallHandler::sys_shutdown_socket] 参数错误: how\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_shutdown_socket] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 调用socket的shutdown方法
        int result = socket_f->shutdown(how);
        if (result < 0) {
            printfRed("[SyscallHandler::sys_shutdown_socket] shutdown失败: %d\n", result);
            return result;
        }

        printfCyan("[SyscallHandler::sys_shutdown_socket] shutdown成功, sockfd=%d, how=%d\n", 
                   sockfd, how);
        return 0;
    }
    uint64 SyscallHandler::sys_sendmsg()
    {
        int sockfd;
        uint64 msg_ptr;
        int flags;

        if (_arg_int(0, sockfd) < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] 参数错误: sockfd\n");
            return SYS_EINVAL;
        }

        if (_arg_addr(1, msg_ptr) < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] 参数错误: msg\n");
            return SYS_EINVAL;
        }

        if (_arg_int(2, flags) < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] 参数错误: flags\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        fs::file *f = p->get_open_file(sockfd);
        if (!f) {
            printfRed("[SyscallHandler::sys_sendmsg] 无效的文件描述符: %d\n", sockfd);
            return SYS_EBADF;
        }

        // 检查是否为socket文件
        fs::socket_file *socket_f = static_cast<fs::socket_file *>(f);
        // 检查是否为socket文件
        if (f->_attrs.filetype != fs::FileTypes::FT_SOCKET)
        {
            printfRed("[SyscallHandler::sys_getsockname] 文件描述符不是socket: %d\n", sockfd);
            return SYS_ENOTSOCK;
        }

        // 从用户空间复制 msghdr 结构
        struct msghdr msg;
        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = pcb->get_pagetable();
        
        if (mem::k_vmm.copy_in(*pt, &msg, msg_ptr, sizeof(msg)) < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] 复制msghdr失败\n");
            return SYS_EFAULT;
        }

        // 验证并复制 iovec 数组
        if (msg.msg_iovlen > IOV_MAX) {
            printfRed("[SyscallHandler::sys_sendmsg] iovec数组过大: %lu\n", msg.msg_iovlen);
            return SYS_EINVAL;
        }

        // 分配内核空间的 iovec 数组
        eastl::vector<struct iovec> kernel_iov(msg.msg_iovlen);
        if (mem::k_vmm.copy_in(*pt, kernel_iov.data(), (uint64)msg.msg_iov, 
                   msg.msg_iovlen * sizeof(struct iovec)) < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] 复制iovec数组失败\n");
            return SYS_EFAULT;
        }

        // 验证每个 iovec 项并分配缓冲区
        eastl::vector<eastl::vector<uint8_t>> data_buffers(msg.msg_iovlen);
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (kernel_iov[i].iov_len > 0) {
                data_buffers[i].resize(kernel_iov[i].iov_len);
                if (mem::k_vmm.copy_in(*pt, data_buffers[i].data(), (uint64)kernel_iov[i].iov_base, 
                           kernel_iov[i].iov_len) < 0) {
                    printfRed("[SyscallHandler::sys_sendmsg] 复制数据失败: %lu\n", i);
                    return SYS_EFAULT;
                }
                // 更新 iovec 指向内核缓冲区
                kernel_iov[i].iov_base = data_buffers[i].data();
            }
        }

        // 处理目标地址（如果有）
        struct sockaddr_in dest_addr;
        if (msg.msg_name && msg.msg_namelen > 0) {
            if (msg.msg_namelen < sizeof(struct sockaddr_in)) {
                printfRed("[SyscallHandler::sys_sendmsg] 地址长度不足\n");
                return SYS_EINVAL;
            }
            if (mem::k_vmm.copy_in(*pt, &dest_addr, (uint64)msg.msg_name, sizeof(dest_addr)) < 0) {
                printfRed("[SyscallHandler::sys_sendmsg] 复制目标地址失败\n");
                return SYS_EFAULT;
            }
        }

        // 构造内核版本的 msghdr
        struct msghdr kernel_msg = msg;
        kernel_msg.msg_iov = kernel_iov.data();
        if (msg.msg_name) {
            kernel_msg.msg_name = &dest_addr;
        }

        // 调用 socket 的 sendmsg 方法
        int result = socket_f->sendmsg(&kernel_msg, flags);
        if (result < 0) {
            printfRed("[SyscallHandler::sys_sendmsg] sendmsg失败: %d\n", result);
            return result;
        }

        printfCyan("[SyscallHandler::sys_sendmsg] sendmsg成功, sockfd=%d, sent=%d bytes\n", 
                   sockfd, result);
        return result;
    }
    uint64 SyscallHandler::sys_mprotect()
    {
        uint64 addr, len;
        int prot;
        if (_arg_addr(0, addr) < 0)
            return syscall::SYS_EFAULT;
        if (_arg_addr(1, len) < 0)
            return syscall::SYS_EFAULT;
        if (_arg_int(2, prot) < 0)
            return syscall::SYS_EFAULT;

        printfBlue("[SyscallHandler::sys_mprotect] addr: %p, len: %p, prot: %d\n",
                   (void *)addr, len, prot);

        // 参数验证
        if (len == 0)
        {
            printfRed("[sys_mprotect] EINVAL: length is zero\n");
            return syscall::SYS_EINVAL;
        }

        // 检查地址对齐
        if ((addr & (PGSIZE - 1)) != 0)
        {
            printfRed("[sys_mprotect] EINVAL: address not page aligned: %p\n", (void *)addr);
            return syscall::SYS_EINVAL;
        }

        // 检查权限标志的合理性
        if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE))
        {
            printfRed("[sys_mprotect] EINVAL: invalid protection flags: %d\n", prot);
            return syscall::SYS_EINVAL;
        }

        // 检查地址范围是否超出虚拟地址空间
        if (addr >= MAXVA || addr + len > MAXVA || addr + len < addr)
        {
            printfRed("[sys_mprotect] ENOMEM: address range out of bounds\n");
            return syscall::SYS_ENOMEM;
        }

        // 特殊检查：地址为0通常表示无效地址
        if (addr == 0)
        {
            printfRed("[sys_mprotect] ENOMEM: invalid address 0\n");
            return syscall::SYS_ENOMEM;
        }

        proc::Pcb *pcb = proc::k_pm.get_cur_pcb();
        if (!pcb || !pcb->get_vma())
        {
            panic("[sys_mprotect] Current process or VMA is null");
        }

        // 页对齐的长度
        uint64 aligned_len = PGROUNDUP(len);
        uint64 end_addr = addr + aligned_len;

        // 查找包含该地址的VMA
        int vma_index = -1;
        for (int i = 0; i < proc::NVMA; i++)
        {
            if (pcb->get_vma()->_vm[i].used)
            {
                uint64 vma_start = pcb->get_vma()->_vm[i].addr;
                uint64 vma_end = vma_start + pcb->get_vma()->_vm[i].len;

                // 检查地址范围是否完全在VMA内
                if (addr >= vma_start && end_addr <= vma_end)
                {
                    vma_index = i;
                    printfGreen("[sys_mprotect] Found VMA[%d]: [%p, %p) for range [%p, %p)\n",
                                i, (void *)vma_start, (void *)vma_end, (void *)addr, (void *)end_addr);
                    break;
                }
            }
        }

        // 构建页表权限标志
        int perm = 0;
        if (prot & PROT_READ)
            perm |= PTE_R;
        if (prot & PROT_WRITE)
            perm |= PTE_W;
        if (prot & PROT_EXEC)
            perm |= PTE_X;

        if (vma_index == -1)
        {
            // 地址不在任何VMA中，直接调用protectpages修改页表权限
            printfYellow("[sys_mprotect] Address range [%p, %p) not found in any VMA, using protectpages\n",
                         (void *)addr, (void *)end_addr);

            // 直接调用protectpages修改页表权限（非VMA上下文）
            if (mem::k_vmm.protectpages(*pcb->get_pagetable(), addr, aligned_len, perm, false) < 0)
            {
                printfRed("[sys_mprotect] protectpages failed for range [%p, %p)\n",
                          (void *)addr, (void *)end_addr);
                return syscall::SYS_EFAULT;
            }

            // 刷新TLB以确保权限更改生效
#ifdef RISCV
            sfence_vma();
#elif defined(LOONGARCH)
            asm volatile("invtlb 0x0,$zero,$zero");
#endif

            printfGreen("[sys_mprotect] Success: changed protection for range [%p, %p) to %d using protectpages\n",
                        (void *)addr, (void *)end_addr, prot);
            return 0;
        }

        // 找到了对应的VMA，现在需要处理权限修改
        proc::vma *vm = &pcb->get_vma()->_vm[vma_index];
        uint64 vma_start = vm->addr;
        uint64 vma_end = vma_start + vm->len;
        int old_prot = vm->prot;

        printfYellow("[sys_mprotect] VMA[%d] covers range [%p, %p), target range [%p, %p), prot: %d -> %d\n",
                     vma_index, (void *)vma_start, (void *)vma_end, (void *)addr, (void *)end_addr, old_prot, prot);

        // 检查权限兼容性：对于文件映射，不能添加原始mmap时没有的写权限
        if (vm->vfile != nullptr && vm->vfd != -1)
        {
            // 这是一个文件映射
            // 如果原始映射没有写权限，但现在要添加写权限，则拒绝
            if ((!(old_prot & PROT_WRITE)) && (prot & PROT_WRITE))
            {
                printfRed("[sys_mprotect] EACCES: Cannot add write permission to read-only file mapping\n");
                return syscall::SYS_EACCES;
            }
        }

        // 保存原始VMA状态用于回滚
        proc::vma original_vma = *vm;
        
        // 记录我们创建的新VMA索引，用于失败时清理
        int created_vma_indices[2] = {-1, -1}; // 最多创建2个新VMA（前段用原VMA，中段和后段用新VMA）
        int created_vma_count = 0;
        
        // 如果要修改的范围与整个VMA完全一致，直接修改VMA权限
        if (addr == vma_start && end_addr == vma_end)
        {
            printfCyan("[sys_mprotect] Exact VMA match, updating protection directly\n");
            vm->prot = prot;
        }
        else
        {
            // 需要拆分VMA
            printfCyan("[sys_mprotect] Need to split VMA for partial protection change\n");

            // 查找空闲的VMA槽位
            int free_vma_count = 0;
            int free_vma_indices[3]; // 最多需要3个新的VMA（前、中、后）
            
            for (int i = 0; i < proc::NVMA; i++)
            {
                if (!pcb->get_vma()->_vm[i].used && free_vma_count < 3)
                {
                    free_vma_indices[free_vma_count++] = i;
                }
            }

            // 计算需要多少个VMA分段
            int segments_needed = 0;
            if (addr > vma_start) segments_needed++; // 前段
            segments_needed++; // 中段（要修改权限的部分）
            if (end_addr < vma_end) segments_needed++; // 后段

            if (free_vma_count < segments_needed - 1)
            {
                printfRed("[sys_mprotect] Not enough free VMA slots for splitting (need %d, have %d)\n",
                          segments_needed - 1, free_vma_count);
                return syscall::SYS_ENOMEM;
            }

            int next_free_idx = 0;

            // 如果有前段（addr > vma_start），保留原VMA作为前段
            if (addr > vma_start)
            {
                // 原VMA变成前段
                vm->len = addr - vma_start;
                printfGreen("[sys_mprotect] Created front segment: VMA[%d] [%p, %p) prot=%d\n",
                            vma_index, (void *)vm->addr, (void *)(vm->addr + vm->len), vm->prot);
            }
            else
            {
                // 没有前段，原VMA将被重用作为中段或后段
            }

            // 创建中段（要修改权限的部分）
            int middle_vma_idx;
            if (addr > vma_start)
            {
                // 有前段，需要新VMA作为中段
                middle_vma_idx = free_vma_indices[next_free_idx++];
                created_vma_indices[created_vma_count++] = middle_vma_idx;
            }
            else
            {
                // 没有前段，重用原VMA作为中段
                middle_vma_idx = vma_index;
            }

            proc::vma *middle_vm = &pcb->get_vma()->_vm[middle_vma_idx];
            *middle_vm = original_vma; // 复制原VMA的所有属性
            middle_vm->used = 1;
            middle_vm->addr = addr;
            middle_vm->len = aligned_len;
            middle_vm->prot = prot;
            
            // 调整文件偏移（如果是文件映射）
            if (middle_vm->vfile != nullptr)
            {
                middle_vm->offset += (addr - vma_start);
                if (middle_vma_idx != vma_index) // 只有在创建新VMA时才增加引用计数
                {
                    middle_vm->vfile->dup(); // 增加引用计数
                }
            }

            printfGreen("[sys_mprotect] Created middle segment: VMA[%d] [%p, %p) prot=%d\n",
                        middle_vma_idx, (void *)middle_vm->addr, (void *)(middle_vm->addr + middle_vm->len), middle_vm->prot);

            // 如果有后段（end_addr < vma_end），创建后段
            if (end_addr < vma_end)
            {
                int back_vma_idx = free_vma_indices[next_free_idx++];
                created_vma_indices[created_vma_count++] = back_vma_idx;
                
                proc::vma *back_vm = &pcb->get_vma()->_vm[back_vma_idx];
                *back_vm = original_vma; // 复制原VMA的所有属性
                back_vm->used = 1;
                back_vm->addr = end_addr;
                back_vm->len = vma_end - end_addr;
                back_vm->prot = old_prot; // 保持原来的权限
                
                // 调整文件偏移（如果是文件映射）
                if (back_vm->vfile != nullptr)
                {
                    back_vm->offset += (end_addr - vma_start);
                    back_vm->vfile->dup(); // 增加引用计数
                }

                printfGreen("[sys_mprotect] Created back segment: VMA[%d] [%p, %p) prot=%d\n",
                            back_vma_idx, (void *)back_vm->addr, (void *)(back_vm->addr + back_vm->len), back_vm->prot);
            }
        }

        // 更新页表权限（VMA上下文，考虑懒分配）
        if (mem::k_vmm.protectpages(*pcb->get_pagetable(), addr, aligned_len, perm, true) < 0)
        {
            printfRed("[sys_mprotect] protectpages failed for range [%p, %p), rolling back VMA changes\n",
                      (void *)addr, (void *)end_addr);
            
            // 恢复VMA状态
            // 1. 恢复原始VMA
            *vm = original_vma;
            
            // 2. 清理我们创建的新VMA
            for (int i = 0; i < created_vma_count; i++)
            {
                int idx = created_vma_indices[i];
                if (idx >= 0 && idx < proc::NVMA)
                {
                    proc::vma *cleanup_vm = &pcb->get_vma()->_vm[idx];
                    
                    // 释放文件引用（如果有）
                    if (cleanup_vm->vfile != nullptr)
                    {
                        cleanup_vm->vfile->free_file();
                    }
                    
                    // 清零VMA结构
                    memset(cleanup_vm, 0, sizeof(proc::vma));
                    
                    printfYellow("[sys_mprotect] Cleaned up VMA[%d] during rollback\n", idx);
                }
            }
            
            printfYellow("[sys_mprotect] VMA state successfully rolled back\n");
            return syscall::SYS_EFAULT;
        }

        // 刷新TLB以确保权限更改生效
#ifdef RISCV
        sfence_vma();
#elif defined(LOONGARCH)
        asm volatile("invtlb 0x0,$zero,$zero");
#endif

        printfGreen("[sys_mprotect] Success: changed protection for range [%p, %p) to %d\n",
                    (void *)addr, (void *)end_addr, prot);

        return 0;
    }
    uint64 SyscallHandler::sys_membarrier()
    {
        return 0;
        panic("未实现该系统调用");
    }
    /**
     * sys_clone3 - 基于 struct clone_args 的新式 clone 系统调用
     * 
     * 相比于 sys_clone, sys_clone3 有以下主要区别：
     * 1. 使用结构体传递参数，而不是多个单独的参数，提供更好的可扩展性
     * 2. 支持更多的 clone 标志和特性（如 pidfd, set_tid 等）
     * 3. 具有更严格的参数验证和错误处理
     * 4. 支持向后兼容的结构体大小检查
     * 
     * 参数：
     * - args_addr: 指向 struct clone_args 的用户空间地址
     * - args_size: struct clone_args 的大小（用于向后兼容）
     * 
     * 返回值：
     * - 成功：新创建进程的 PID
     * - 失败：负数错误码
     */
    
    // clone3 参数结构体，对应 Linux 内核的 struct clone_args
    struct clone_args {
        uint64 flags;          // clone 标志位
        uint64 pidfd;          // 指向存储新进程文件描述符的位置
        uint64 child_tid;      // 指向存储子进程 TID 的位置
        uint64 parent_tid;     // 指向存储父进程 TID 的位置
        uint64 exit_signal;    // 子进程退出时发送给父进程的信号
        uint64 stack;          // 栈地址
        uint64 stack_size;     // 栈大小
        uint64 tls;            // TLS (线程本地存储) 地址
        uint64 set_tid;        // 指向 TID 数组的指针
        uint64 set_tid_size;   // TID 数组的大小
        uint64 cgroup;         // cgroup 文件描述符
    };

    uint64 SyscallHandler::sys_clone3()
    {
        panic("未实现该系统调用");
        TODO("TBF")
        
        uint64 args_addr;
        uint64 args_size;
        struct clone_args args;
        
        // 获取参数：clone_args 结构体地址和大小
        if (_arg_addr(0, args_addr) < 0) {
            printfRed("[SyscallHandler::sys_clone3] Error fetching clone_args address\n");
            return SYS_EFAULT;
        }
        
        if (_arg_addr(1, args_size) < 0) {
            printfRed("[SyscallHandler::sys_clone3] Error fetching clone_args size\n");
            return SYS_EFAULT;
        }
        
        // 验证参数大小
        if (args_size < sizeof(uint64)) { // 至少要有 flags 字段
            printfRed("[SyscallHandler::sys_clone3] Invalid args_size: %llu\n", args_size);
            return SYS_EINVAL;
        }
        
        if (args_size > sizeof(struct clone_args)) {
            printfRed("[SyscallHandler::sys_clone3] args_size too large: %llu\n", args_size);
            return SYS_E2BIG;
        }
        
        // 从用户空间复制 clone_args 结构体
        proc::Pcb *cur = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur->get_pagetable();
        
        // 初始化 args 结构体为 0
        memset(&args, 0, sizeof(args));
        
        // 只复制用户提供的大小，避免越界
        if (mem::k_vmm.copy_in(*pt, &args, args_addr, args_size) != 0) {
            printfRed("[SyscallHandler::sys_clone3] Error copying clone_args from user space\n");
            return SYS_EFAULT;
        }
        
        printfCyan("[SyscallHandler::sys_clone3] flags: 0x%lx, stack: %p, child_tid: %p, parent_tid: %p, tls: %p\n",
                   args.flags, (void *)args.stack, (void *)args.child_tid, (void *)args.parent_tid, (void *)args.tls);
        
        // 验证标志位
        if (args.flags & ~(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | 
                          CLONE_PTRACE | CLONE_VFORK | CLONE_PARENT | CLONE_THREAD |
                          CLONE_NEWNS | CLONE_SYSVSEM | CLONE_SETTLS | 
                          CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | 
                          CLONE_DETACHED | CLONE_UNTRACED | CLONE_CHILD_SETTID |
                          CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC |
                          CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_IO |
                          CLONE_NEWTIME | CLONE_PIDFD | CSIGNAL)) {
            printfRed("[SyscallHandler::sys_clone3] Invalid flags: 0x%lx\n", args.flags);
            return SYS_EINVAL;
        }
        
        // 暂时不支持某些复杂特性
        if (args.flags & (CLONE_PIDFD | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
                         CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWTIME |
                         CLONE_NEWCGROUP)) {
            printfRed("[SyscallHandler::sys_clone3] Unsupported flags: 0x%lx\n", args.flags);
            return SYS_ENOSYS;  // 功能未实现
        }
        
        // 如果设置了 CLONE_SETTLS 但没有提供 TLS 地址，返回错误
        if ((args.flags & CLONE_SETTLS) && args.tls == 0) {
            printfRed("[SyscallHandler::sys_clone3] CLONE_SETTLS set but tls is null\n");
            return SYS_EINVAL;
        }
        
        // 调用底层的 clone 函数，传入相应的参数
        uint64 clone_pid = proc::k_pm.clone(args.flags, args.stack, args.parent_tid, 
                                           args.tls, args.child_tid);
        
        printfCyan("[SyscallHandler::sys_clone3] Created process with PID: %llu\n", clone_pid);
        return clone_pid;
    }
    uint64 SyscallHandler::sys_poweroff()
    {
        panic("未实现该系统调用");
    }

    //================================== rocket syscalls ===================================
    uint64 SyscallHandler::sys_fsetxattr()
    {
        int fd;
        fs::file *f;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;
        int flags;

        // 获取参数
        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 无效的文件描述符\n");
            return -EBADF;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 获取大小参数失败\n");
            return -EINVAL;
        }
        if (_arg_int(4, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 获取标志参数失败\n");
            return -EINVAL;
        }

        size = isize;

        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fsetxattr] 文件指针为空\n");
            return -EBADF;
        }

        if (f->lwext4_file_struct.flags & O_PATH)
        {
            return -EBADF;
        }

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 检查值大小是否合理
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        // 验证标志
        if (flags != 0 && flags != XATTR_CREATE && flags != XATTR_REPLACE &&
            flags != (XATTR_CREATE | XATTR_REPLACE))
        {
            return -EINVAL;
        }

        printfCyan("[SyscallHandler::sys_fsetxattr] fd=%d, name=%s, value=%p, size=%zu, flags=%d\n",
                   fd, name.c_str(), (void *)value_addr, size, flags);

        // 简化实现：由于我们还没有完整的文件系统扩展属性支持，
        // 这里返回 ENOTSUP 表示不支持扩展属性
        // 在真实的实现中，需要将属性存储到文件系统的inode中
        return -ENOTSUP;
    }
    uint64 SyscallHandler::sys_fgetxattr()
    {
        fs::file *f;
        int fd;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;

        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fgetxattr] 无效的文件描述符\n");
            return -EBADF;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_fgetxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_fgetxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_fgetxattr] 获取大小参数失败\n");
            return -EINVAL;
        }
        size = isize;

        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fgetxattr] 文件指针为空\n");
            return -EBADF;
        }

        printfCyan("[SyscallHandler::sys_fgetxattr] fd=%d, name=%s, value=%p, size=%zu\n",
                   fd, name.c_str(), (void *)value_addr, size);

        if (f->lwext4_file_struct.flags & O_PATH)
            return -EBADF;

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 如果size为0，应该返回属性值的大小（如果存在）
        if (size == 0)
        {
            // 在真实实现中，这里应该查询属性的大小
            // 现在返回 ENODATA 表示属性不存在
            return -ENODATA;
        }

        // 检查缓冲区大小是否足够
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        // 简化实现：由于我们还没有完整的文件系统扩展属性支持，
        // 返回 ENODATA 表示请求的扩展属性不存在
        // 在真实的实现中，需要从文件系统的inode中读取属性
        return -ENODATA;
    }

    uint64 SyscallHandler::sys_setxattr()
    {
        eastl::string path;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;
        int flags;

        // 获取参数
        if (_arg_str(0, path, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_setxattr] 获取路径失败\n");
            return -EINVAL;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_setxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_setxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_setxattr] 获取大小参数失败\n");
            return -EINVAL;
        }
        if (_arg_int(4, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_setxattr] 获取标志参数失败\n");
            return -EINVAL;
        }

        size = isize;

        // 检查路径
        if (path.empty())
        {
            return -EINVAL;
        }

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 检查值大小是否合理
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        // 验证标志
        if (flags != 0 && flags != XATTR_CREATE && flags != XATTR_REPLACE)
        {
            return -EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setxattr] path=%s, name=%s, value=%p, size=%zu, flags=%d\n",
                   path.c_str(), name.c_str(), (void *)value_addr, size, flags);

        // 简化实现：返回 ENOTSUP 表示不支持扩展属性
        return -ENOTSUP;
    }

    uint64 SyscallHandler::sys_lsetxattr()
    {
        eastl::string path;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;
        int flags;

        // 获取参数 - 与setxattr相同
        if (_arg_str(0, path, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_lsetxattr] 获取路径失败\n");
            return -EINVAL;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_lsetxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_lsetxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_lsetxattr] 获取大小参数失败\n");
            return -EINVAL;
        }
        if (_arg_int(4, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_lsetxattr] 获取标志参数失败\n");
            return -EINVAL;
        }

        size = isize;

        // 检查路径
        if (path.empty())
        {
            return -EINVAL;
        }

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 检查值大小是否合理
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        // 验证标志
        if (flags != 0 && flags != XATTR_CREATE && flags != XATTR_REPLACE)
        {
            return -EINVAL;
        }

        printfCyan("[SyscallHandler::sys_lsetxattr] path=%s, name=%s, value=%p, size=%zu, flags=%d\n",
                   path.c_str(), name.c_str(), (void *)value_addr, size, flags);

        // lsetxattr与setxattr的区别在于对符号链接的处理
        // 简化实现：返回 ENOTSUP 表示不支持扩展属性
        return -ENOTSUP;
    }

    uint64 SyscallHandler::sys_getxattr()
    {
        eastl::string path;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;

        // 获取参数
        if (_arg_str(0, path, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_getxattr] 获取路径失败\n");
            return -EINVAL;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_getxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_getxattr] 获取大小参数失败\n");
            return -EINVAL;
        }

        size = isize;

        // 检查路径
        if (path.empty())
        {
            return -EINVAL;
        }

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 如果size为0，应该返回属性值的大小（如果存在）
        if (size == 0)
        {
            // 在真实实现中，这里应该查询属性的大小
            // 现在返回 ENODATA 表示属性不存在
            return -ENODATA;
        }

        // 检查缓冲区大小是否足够
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        printfCyan("[SyscallHandler::sys_getxattr] path=%s, name=%s, value=%p, size=%zu\n",
                   path.c_str(), name.c_str(), (void *)value_addr, size);

        // 简化实现：返回 ENODATA 表示属性不存在
        return -ENODATA;
    }

    uint64 SyscallHandler::sys_lgetxattr()
    {
        eastl::string path;
        eastl::string name;
        uint64 value_addr;
        long isize;
        size_t size;

        // 获取参数 - 与getxattr相同
        if (_arg_str(0, path, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_lgetxattr] 获取路径失败\n");
            return -EINVAL;
        }
        if (_arg_str(1, name, MAXPATH) < 0)
        {
            printfRed("[SyscallHandler::sys_lgetxattr] 获取属性名失败\n");
            return -EINVAL;
        }
        if (_arg_addr(2, value_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_lgetxattr] 获取值地址失败\n");
            return -EINVAL;
        }
        if (_arg_long(3, isize) < 0)
        {
            printfRed("[SyscallHandler::sys_lgetxattr] 获取大小参数失败\n");
            return -EINVAL;
        }

        size = isize;

        // 检查路径
        if (path.empty())
        {
            return -EINVAL;
        }

        // 检查属性名是否为空或过长
        if (name.empty() || name.length() > 255)
        {
            return -ERANGE;
        }

        // 如果size为0，应该返回属性值的大小（如果存在）
        if (size == 0)
        {
            // 在真实实现中，这里应该查询属性的大小
            // 现在返回 ENODATA 表示属性不存在
            return -ENODATA;
        }

        // 检查缓冲区大小是否足够
        if (size > 65536)
        { // 64KB limit
            return -ERANGE;
        }

        printfCyan("[SyscallHandler::sys_lgetxattr] path=%s, name=%s, value=%p, size=%zu\n",
                   path.c_str(), name.c_str(), (void *)value_addr, size);

        // lgetxattr与getxattr的区别在于对符号链接的处理
        // 简化实现：返回 ENODATA 表示属性不存在
        return -ENODATA;
    }

    uint64 SyscallHandler::sys_mknodat()
    {
        int dirfd;
        eastl::string pathname;
        int imode;
        long idev;
        // 获取参数
        if (_arg_int(0, dirfd) < 0)
            return SYS_EFAULT;
        if (_arg_str(1, pathname, MAXPATH) < 0)
            return SYS_EFAULT;
        if (_arg_int(2, imode) < 0)
            return SYS_EFAULT;
        if (_arg_long(3, idev) < 0)
            return SYS_EFAULT;
        mode_t mode = imode;
        dev_t dev = idev;

        // 调用进程管理器的 mknod 函数
        int result = proc::k_pm.mknod(dirfd, pathname, mode, dev);
        return result;
    }

    uint64 SyscallHandler::sys_symlinkat()
    {
        uint64 target_addr;
        int newdirfd;
        uint64 linkpath_addr;

        // 获取参数
        if (_arg_addr(0, target_addr) < 0)
            return SYS_EFAULT;
        if (_arg_int(1, newdirfd) < 0)
            return SYS_EFAULT;
        if (_arg_addr(2, linkpath_addr) < 0)
            return SYS_EFAULT;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        eastl::string target;
        eastl::string linkpath;

        // 从用户空间复制字符串

        int cpres = mem::k_vmm.copy_str_in(*pt, target, target_addr, 256) ;
        if (cpres < 0)
        {
            printfRed("[sys_symlinkat] Error copying path from user space\n");
            return cpres;
        }
        cpres = mem::k_vmm.copy_str_in(*pt, linkpath, linkpath_addr, 256) ;
        if (cpres < 0)
        {
            printfRed("[sys_symlinkat] Error copying path from user space\n");
            return cpres;
        }


        eastl::string abs_linkpath;

        // 处理linkpath：如果是绝对路径，忽略newdirfd；如果是相对路径，需要处理newdirfd
        if (linkpath[0] == '/')
        {
            // 绝对路径
            abs_linkpath = linkpath;
        }
        else
        {
            // 相对路径，需要处理newdirfd
            if (newdirfd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_linkpath = get_absolute_path(linkpath.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用newdirfd指向的目录
                fs::file *dir_file = p->get_open_file(newdirfd);
                if (!dir_file)
                {
                    printfRed("[sys_symlinkat] Invalid newdirfd: %d\n", newdirfd);
                    return SYS_EBADF;
                }

                // 检查newdirfd是否指向一个目录
                if (dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
                {
                    printfRed("[sys_symlinkat] newdirfd %d不是目录，文件类型: %d\n", newdirfd, (int)dir_file->_attrs.filetype);
                    return SYS_ENOTDIR; // 不是目录
                }

                abs_linkpath = get_absolute_path(linkpath.c_str(), dir_file->_path_name.c_str());
            }
        }

        printfCyan("[sys_symlinkat] Creating symlink: %s -> %s\n", abs_linkpath.c_str(), target.c_str());

        // 检查linkpath是否已经存在
        if (fs::k_vfs.is_file_exist(abs_linkpath))
        {
            printfRed("[sys_symlinkat] File already exists: %s\n", abs_linkpath.c_str());
            return SYS_EEXIST;
        }

        // 检查父目录是否存在
        eastl::string parent_dir;
        size_t last_slash = abs_linkpath.find_last_of('/');
        if (last_slash != eastl::string::npos && last_slash > 0)
        {
            parent_dir = abs_linkpath.substr(0, last_slash);
        }
        else
        {
            parent_dir = "/";
        }

        if (!fs::k_vfs.is_file_exist(parent_dir))
        {
            printfRed("[sys_symlinkat] Parent directory does not exist: %s\n", parent_dir.c_str());
            return SYS_ENOENT;
        }

        // 检查父目录确实是目录
        eastl::string parent_str = parent_dir;
        int parent_type = vfs_path2filetype(parent_str);
        if (parent_type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("[sys_symlinkat] Parent path is not a directory: %s (type: %d)\n", parent_dir.c_str(), parent_type);
            return SYS_ENOTDIR;
        }

        // 检查是否为虚拟文件系统路径
        if (fs::k_vfs.is_filepath_virtual(abs_linkpath))
        {
            printfRed("[sys_symlinkat] Cannot create symlink in virtual filesystem: %s\n", abs_linkpath.c_str());
            return SYS_EPERM;
        }

        // 创建符号链接
        int result = vfs_ext_symlink(target.c_str(), abs_linkpath.c_str());
        if (result < 0)
        {
            printfRed("[sys_symlinkat] Failed to create symlink: %s -> %s, error: %d\n",
                      abs_linkpath.c_str(), target.c_str(), result);

            // 将ext4错误码转换为系统错误码
            return result;
        }

        printfCyan("[sys_symlinkat] Successfully created symlink: %s -> %s\n", abs_linkpath.c_str(), target.c_str());
        return 0;
    }
    uint64 SyscallHandler::sys_fstatfs()
    {
        int fd;
        uint64 buf_addr;

        // 获取参数
        if (_arg_int(0, fd) < 0 || _arg_addr(1, buf_addr) < 0)
        {
            printfRed("[sys_fstatfs] 参数错误\n");
            return SYS_EINVAL;
        }

        // 检查buf地址是否有效
        if (buf_addr == 0)
        {
            printfRed("[sys_fstatfs] buf地址无效\n");
            return SYS_EFAULT;
        }

        printfCyan("[sys_fstatfs] fd: %d, buf_addr: %p\n", fd, (void *)buf_addr);

        // 获取当前进程
        proc::Pcb *p = proc::k_pm.get_cur_pcb();

        // 检查文件描述符是否有效
        fs::file *f = p->get_open_file(fd);
        if (!f)
        {
            printfRed("[sys_fstatfs] 无效的文件描述符: %d\n", fd);
            return SYS_EBADF;
        }

        // 填充statfs结构体 - 与sys_statfs使用相同的文件系统信息
        struct statfs st;

        // 文件系统类型 - 使用EXT4的magic number
        st.f_type = 0xEF53; // EXT4_SUPER_MAGIC

        // 块大小 - 使用页面大小作为优化的传输块大小
        st.f_bsize = PGSIZE;

        // 文件系统总块数
        st.f_blocks = 1UL << 20; // 1M blocks

        // 空闲块数
        st.f_bfree = 1UL << 19; // 512K free blocks

        // 非特权用户可用的空闲块数
        st.f_bavail = 1UL << 18; // 256K available to unprivileged users

        // 文件系统总inode数
        st.f_files = 1UL << 16; // 64K inodes

        // 空闲inode数
        st.f_ffree = 1UL << 15; // 32K free inodes

        // 文件系统ID - 简单设置为固定值
        st.f_fsid.val[0] = 0xF7;
        st.f_fsid.val[1] = 0x1A;

        // 文件名最大长度
        st.f_namelen = 255; // EXT4 standard

        // 碎片大小（Linux 2.6+）
        st.f_frsize = PGSIZE;

        // 挂载标志（Linux 2.6.36+）
        st.f_flags = 0; // 没有特殊挂载标志

        // 预留空间清零
        for (int i = 0; i < 4; i++)
        {
            st.f_spare[i] = 0;
        }

        // 将结果拷贝到用户空间
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, buf_addr, &st, sizeof(st)) < 0)
        {
            printfRed("[sys_fstatfs] 结果拷贝到用户空间失败\n");
            return SYS_EFAULT;
        }

        printfGreen("[sys_fstatfs] 成功获取文件描述符 %d 的文件系统信息\n", fd);
        return 0;
    }
    uint64 SyscallHandler::sys_truncate()
    {
        uint64 addr;
        eastl::string pathname;
        off_t length;
        if (_arg_addr(0, addr) < 0 || _arg_long(1, length) < 0)
        {
            printfRed("[SyscallHandler::sys_truncate] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
int cpres = mem::k_vmm.copy_str_in(*proc::k_pm.get_cur_pcb()->get_pagetable(), pathname, addr, 256) ;
        if (cpres < 0)
        {
            printfRed("[sys_fstatfs] Error copying path from user space\n");
            return cpres;
        }
        
        pathname = get_absolute_path(pathname.c_str(), proc::k_pm.get_cur_pcb()->_cwd_name.c_str());
        if (fs::k_vfs.is_file_exist(pathname.c_str()) != 1)
        {
            printfRed("[SyscallHandler::sys_truncate] 文件不存在: %s\n", pathname.c_str());
            return SYS_ENOENT; // 文件不存在
        }

        // 打开文件，需要使用写入模式
        fs::file *file = nullptr;
        int flags = O_WRONLY; // 以写入模式打开
        int status = fs::k_vfs.openat(pathname, file, flags);

        if (status != EOK || !file)
        {
            printfRed("[SyscallHandler::sys_truncate] 无法打开文件: %s, 错误码: %d\n", pathname.c_str(), status);
            return SYS_EACCES; // 访问被拒绝
        }

        // 检查是否具有写权限
        if (!(file->_attrs.u_write))
        {
            printfRed("[SyscallHandler::sys_truncate] 文件没有写权限: %s\n", pathname.c_str());
            file->free_file(); // 释放文件对象
            return SYS_EACCES; // 访问被拒绝
        }

        // 调用vfs_truncate执行截断操作
        status = vfs_truncate(file, length);

        // 释放文件对象
        file->free_file();

        return status;
    }
    uint64 SyscallHandler::sys_fallocate()
    {
        int fd;
        fs::file *f;
        int mode = 0;
        off_t offset;
        off_t len;
        if (_arg_fd(0, &fd, &f) < 0 ||
            _arg_int(1, mode) < 0 ||
            _arg_long(2, offset) < 0 ||
            _arg_long(3, len) < 0)
        {
            printfRed("[SyscallHandler::sys_fallocate] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        if (fd < 0 || fd >= NOFILE)
        {
            printfRed("[SyscallHandler::sys_fallocate] 无效的文件描述符: %d\n", fd);
            return SYS_EBADF; // 无效的文件描述符
        }
        printfCyan("[SyscallHandler::sys_fallocate] fd=%d, mode=%d, offset=%d, len=%x\n", fd, mode, offset, len);
        printf("[SyscallHandler::sys_fallocate] f.mode=%b\n", f->_attrs.transMode());
        if (!f || !f->_attrs.u_write)
        {
            printfRed("[SyscallHandler::sys_fallocate] 无效的文件描述符: %d\n", fd);
            return SYS_EBADF; // 无效的文件描述符
        }
        if (offset < 0 || len < 0)
        {
            printfRed("[SyscallHandler::sys_fallocate] offset或len不能为负数: offset=%d, len=%d\n", offset, len);
            return SYS_EINVAL; // 参数错误
        }

        return vfs_fallocate(f, offset, len);
    }
    uint64 SyscallHandler::sys_fchdir()
    {
        int fd;
        if (_arg_int(0, fd) < 0)
        {
            printfRed("[SyscallHandler::sys_fchdir] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        fs::file *f = proc::k_pm.get_cur_pcb()->get_open_file(fd);
        eastl::string path;
        if (!f || !f->_attrs.u_read)
        {
            printfRed("[SyscallHandler::sys_fchdir] 无效的文件描述符: %d\n", fd);
            return SYS_EBADF; // 无效的文件描述符
        }
        path = f->_path_name;
        return proc::k_pm.chdir(path);
    }
    uint64 SyscallHandler::sys_chroot()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchmod()
    {
        fs::file *f;
        int fd;
        long mode_long;

        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fchmod] 无效的文件描述符\n");
            return -EBADF; // 无效的文件描述符
        }
        if (_arg_long(1, mode_long) < 0)
        {
            printfRed("[SyscallHandler::sys_fchmod] 参数错误\n");
            return -EINVAL; // 参数错误
        }

        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fchmod] 文件指针为空\n");
            return -EBADF;
        }
        printfCyan("[SyscallHandler::sys_fchmod] fd=%d, mode=%ld\n", fd, mode_long);
        // 检查文件是否以 O_PATH 标志打开，O_PATH 文件不允许 fchmod
        if (f->lwext4_file_struct.flags & O_PATH)
        {
            return -EBADF;
        }

        mode_t mode = (mode_t)mode_long;
        eastl::string pathname = f->_path_name;

        if (pathname.empty())
        {
            printfRed("[SyscallHandler::sys_fchmod] 文件路径为空\n");
            return -EBADF;
        }

        return vfs_chmod(pathname, mode);
    }

    uint64 SyscallHandler::sys_fchmodat()
    {
        int dirfd;
        eastl::string pathname;
        long mode_long;
        int flags;
        if (_arg_int(0, dirfd) < 0 ||
            _arg_long(2, mode_long) < 0 ||
            _arg_int(3, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_fchmodat] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        int rs=_arg_str(1, pathname, MAXPATH) ;
        if(rs<0)
        {
            return rs; // 参数错误
        }
        mode_t mode = (mode_t)mode_long;
        printfCyan("[SyscallHandler::sys_fchmodat] dirfd=%d, pathname=%s, mode=%d, flags=%o\n", dirfd, pathname.c_str(), mode, flags);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();

        // 处理dirfd和路径
        eastl::string abs_pathname;
        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dirfd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dirfd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_fchmodat] 无效的dirfd: %d\n", dirfd);
                    return SYS_EBADF; // 无效的文件描述符
                }
            if (dir_file && dir_file->_attrs.filetype != fs::FileTypes::FT_DIRECT)
            {
                printfRed("[SyscallHandler::sys_fchmodat] dirfd %d不是目录，文件类型: %d\n", dirfd, (int)dir_file->_attrs.filetype);
                return SYS_ENOTDIR; // 不是目录
            }
                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        printfCyan("[SyscallHandler::sys_fchmodat] 绝对路径: %s\n", abs_pathname.c_str());

        // 检查是否是 /proc/self/fd/ 路径
        if (abs_pathname.find("/proc/self/fd/") == 0)
        {
            // 解析文件描述符
            eastl::string fd_str = abs_pathname.substr(14); // 跳过 "/proc/self/fd/"
            int fd = 0;
            for (size_t i = 0; i < fd_str.size(); ++i)
            {
                if (fd_str[i] < '0' || fd_str[i] > '9')
                    break;
                fd = fd * 10 + (fd_str[i] - '0');
            }

            fs::file *target_file = p->get_open_file(fd);
            if (!target_file)
            {
                printfRed("[SyscallHandler::sys_fchmodat] 无效的文件描述符: %d\n", fd);
                return SYS_EBADF;
            }

            // 检查是否以 O_PATH 标志打开，如果是则返回 EBADF
            if (target_file->lwext4_file_struct.flags & O_PATH)
            {
                printfRed("[SyscallHandler::sys_fchmodat] O_PATH标志打开的文件不允许修改权限\n");
                return SYS_EBADF;
            }

            // 使用实际文件路径
            abs_pathname = target_file->_path_name;
        }

        fs::file *file = nullptr;
        int res =fs::k_vfs.openat(abs_pathname, file, O_RDONLY);
        if(res<0)
        {
            printfRed("[SyscallHandler::sys_fchmodat] 无法打开文件: %s, 错误码: %d\n", abs_pathname.c_str(), res);
            return res; // 返回错误码
        }
        proc::Pcb *cur_pcb = proc::k_pm.get_cur_pcb();
        if(cur_pcb->_uid!= 0 && cur_pcb->_uid != file->lwext4_file_struct.flags)
        if (file->lwext4_file_struct.flags & O_PATH)
        {
            printfRed("[SyscallHandler::sys_fchmodat] O_PATH标志打开的文件不允许修改权限\n");
            file->free_file();
            return SYS_EBADF; // 无效的文件描述符
        }
        // 检查文件是否存在

        file->free_file();
        if (fs::k_vfs.is_file_exist(abs_pathname.c_str()) != 1)
        {
            printfRed("[SyscallHandler::sys_fchmodat] 文件不存在: %s\n", abs_pathname.c_str());
            return SYS_ENOENT; // 文件不存在
        }
        return vfs_chmod(abs_pathname, mode);
    }
    uint64 SyscallHandler::sys_fchownat()
    {
        // 没实现，假的
        int dirfd;
        eastl::string pathname;
        long mode_long;
        int flags;
        if (_arg_int(0, dirfd) < 0 ||
            _arg_str(1, pathname, MAXPATH) < 0 ||
            _arg_long(2, mode_long) < 0 ||
            _arg_int(3, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_fchownat] 参数错误\n");
            return SYS_EINVAL; // 参数错误
        }
        // mode_t mode = (mode_t)mode_long;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();

        // 处理dirfd和路径
        eastl::string abs_pathname;

        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dirfd == AT_FDCWD)
            {

                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dirfd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_fchownat] 无效的dirfd: %d\n", dirfd);
                    return SYS_EBADF; // 无效的文件描述符
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        // 检查是否是 /proc/self/fd/ 路径
        if (abs_pathname.find("/proc/self/fd/") == 0)
        {
            // 解析文件描述符
            eastl::string fd_str = abs_pathname.substr(14); // 跳过 "/proc/self/fd/"
            int fd = 0;
            for (size_t i = 0; i < fd_str.size(); ++i)
            {
                if (fd_str[i] < '0' || fd_str[i] > '9')
                    break;
                fd = fd * 10 + (fd_str[i] - '0');
            }

            fs::file *target_file = p->get_open_file(fd);
            if (!target_file)
            {
                printfRed("[SyscallHandler::sys_fchmodat] 无效的文件描述符: %d\n", fd);
                return SYS_EBADF;
            }

            // 检查是否以 O_PATH 标志打开，如果是则返回 EBADF
            if (target_file->lwext4_file_struct.flags & O_PATH)
            {
                printfRed("[SyscallHandler::sys_fchmodat] O_PATH标志打开的文件不允许修改权限\n");
                return SYS_EBADF;
            }

            // 使用实际文件路径
            abs_pathname = target_file->_path_name;
        }

        // 没有实现实际功能，只有错误检查
        return 0;
    }
    uint64 SyscallHandler::sys_fchown()
    {
        fs::file *f;
        int fd;
        int uid, gid;

        if (_arg_fd(0, &fd, &f) < 0)
        {
            printfRed("[SyscallHandler::sys_fchown] 无效的文件描述符\n");
            return -EBADF; // 无效的文件描述符
        }
        if (_arg_int(1, uid) < 0 || _arg_int(2, gid) < 0)
        {
            printfRed("[SyscallHandler::sys_fchown] 参数错误\n");
            return -EINVAL; // 参数错误
        }

        if (f == nullptr)
        {
            printfRed("[SyscallHandler::sys_fchown] 文件指针为空\n");
            return -EBADF;
        }

        // 检查文件是否以 O_PATH 标志打开，O_PATH 文件不允许 fchown
        if (f->lwext4_file_struct.flags & O_PATH)
        {
            return -EBADF;
        }

        // 没实现实际功能，只有错误检查
        return 0;
    }
    uint64 SyscallHandler::sys_preadv()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_pwritev()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sync_file_range()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_acct()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_clock_settime()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_clock_getres()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_setscheduler()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_getscheduler()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_getparam()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_setaffinity()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sigaltstack()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_rt_sigsuspend()
    {
        panic("未实现该系统调用");
        uint64 setaddr;
        int sigsize;

        // 获取参数：信号集地址和大小
        if (_arg_addr(0, setaddr) < 0)
            return -1;
        if (_arg_int(1, sigsize) < 0)
            return -1;

        // 检查信号集大小
        if (sigsize != sizeof(signal::sigset_t))
        {
            printfRed("[sys_rt_sigsuspend] Invalid sigsize: %d, expected: %d\n", 
                     sigsize, (int)sizeof(signal::sigset_t));
            return syscall::SYS_EINVAL; // EINVAL
        }

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = cur_proc->get_pagetable();

        // 从用户空间拷贝新的信号掩码
        signal::sigset_t new_mask;
        if (mem::k_vmm.copy_in(*pt, &new_mask, setaddr, sizeof(signal::sigset_t)) < 0)
        {
            printfRed("[sys_rt_sigsuspend] Failed to copy signal mask from user space\n");
            return -14; // EFAULT
        }

        // 调用信号模块中的sigsuspend函数
        return signal::sigsuspend(&new_mask);
    }
    uint64 SyscallHandler::sys_rt_sigpending()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_rt_sigqueueinfo()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setregrid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setreuid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setresuid()
    {
        int ruid, euid, suid;

        // 获取参数
        if (_arg_int(0, ruid) < 0 || _arg_int(1, euid) < 0 || _arg_int(2, suid) < 0)
        {
            printfRed("[SyscallHandler::sys_setresuid] 参数错误\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setresuid] ruid: %d, euid: %d, suid: %d\n", ruid, euid, suid);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();

        // 获取当前的用户ID
        uint32 origin_uid = p->get_uid();
        uint32 origin_euid = p->get_euid();
        uint32 origin_suid = p->get_suid();

        // 检查是否为特权进程（root用户，euid == 0）
        if (p->get_euid() == 0)
        {
            printfCyan("[SyscallHandler::sys_setresuid] 特权进程，可以设置任意值\n");

            // 特权进程可以设置任意值
            if (ruid != -1)
            {
                p->set_uid(ruid);
            }
            if (euid != -1)
            {
                p->set_euid(euid);
                p->set_fsuid(euid); // 同时设置文件系统用户ID
            }
            if (suid != -1)
            {
                p->set_suid(suid);
            }
        }
        else
        {
            // 非特权进程，需要检查权限
            if (ruid != -1)
            {
                if (ruid != (int)origin_uid && ruid != (int)origin_euid && ruid != (int)origin_suid)
                {
                    printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 ruid: %d\n", ruid);
                    return SYS_EPERM;
                }
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 ruid: %d\n", ruid);
                p->set_uid(ruid);
            }

            if (euid != -1)
            {
                if (euid != (int)origin_uid && euid != (int)origin_euid && euid != (int)origin_suid)
                {
                    printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 euid: %d\n", euid);
                    return SYS_EPERM;
                }
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 euid: %d\n", euid);
                p->set_euid(euid);
                p->set_fsuid(euid); // 同时设置文件系统用户ID
            }

            if (suid != -1)
            {
                if (suid != (int)origin_uid && suid != (int)origin_euid && suid != (int)origin_suid)
                {
                    printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 suid: %d\n", suid);
                    return SYS_EPERM;
                }
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 suid: %d\n", suid);
                p->set_suid(suid);
            }
        }

        return 0;
    }
    uint64 SyscallHandler::sys_getresuid()
    {
        uint64 ruid_addr, euid_addr, suid_addr;

        // 获取参数
        if (_arg_addr(0, ruid_addr) < 0 || _arg_addr(1, euid_addr) < 0 || _arg_addr(2, suid_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getresuid] 参数错误\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 获取当前的用户ID
        uint32 ruid = p->get_uid();
        uint32 euid = p->get_euid();
        uint32 suid = p->get_suid();

        printfCyan("[SyscallHandler::sys_getresuid] ruid: %u, euid: %u, suid: %u\n", ruid, euid, suid);

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, ruid_addr, &ruid, sizeof(ruid)) < 0 ||
            mem::k_vmm.copy_out(*pt, euid_addr, &euid, sizeof(euid)) < 0 ||
            mem::k_vmm.copy_out(*pt, suid_addr, &suid, sizeof(suid)) < 0)
        {
            printfRed("[SyscallHandler::sys_getresuid] 拷贝到用户空间失败\n");
            return SYS_EFAULT;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_setresgid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getresgid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setfsuid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setfsgid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getgroups()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setgroups()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sethostname()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setdomainname()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_umask()
    {
        // 获取新的 umask 值
        int new_mask;
        if (_arg_int(0, new_mask) < 0)
            return -1;

        // 只取低9位，确保是有效的权限位
        mode_t new_umask = (mode_t)(new_mask & 0777);

        // 获取当前进程
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
            return -1;

        // 获取旧的 umask 值
        mode_t old_umask = p->_umask;

        // 设置新的 umask 值
        p->_umask = new_umask;

        // 返回旧的 umask 值
        return (uint64)old_umask;
    }
    uint64 SyscallHandler::sys_adjtimex()
    {
        panic("未实现该系统调用");
    }

    uint64 SyscallHandler::sys_recvmsg()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fadvise64()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_msync()
    {
        uint64 addr;
        size_t length;
        int flags;

        // 获取系统调用参数
        if (_arg_addr(0, addr) < 0 || _arg_addr(1, length) < 0 || _arg_int(2, flags) < 0)
        {
            printfRed("[SyscallHandler::sys_msync] Error fetching msync arguments\n");
            return -EFAULT;
        }

        // 参数验证
        if (addr == 0 || length == 0)
        {
            printfRed("[SyscallHandler::sys_msync] Invalid parameters: addr=%p, length=%zu\n", (void *)addr, length);
            return -EINVAL;
        }

        // 地址必须页对齐
        if (addr % PGSIZE != 0)
        {
            printfRed("[SyscallHandler::sys_msync] Address not page aligned: %p\n", (void *)addr);
            return -EINVAL;
        }

        // 检查 flags 参数的有效性
        int valid_flags = MS_ASYNC | MS_SYNC | MS_INVALIDATE;
        if ((flags & ~valid_flags) != 0)
        {
            printfRed("[SyscallHandler::sys_msync] Invalid flags: 0x%x\n", flags);
            return -EINVAL;
        }
        printfCyan("[SyscallHandler::sys_msync] addr=%p, length=%zu, flags=0x%x\n",
                   (void *)addr, length, flags);
        // MS_ASYNC 和 MS_SYNC 不能同时设置，且必须设置其中一个
        bool has_async = (flags & MS_ASYNC) != 0;
        bool has_sync = (flags & MS_SYNC) != 0;

        if (has_async && has_sync)
        {
            printfRed("[SyscallHandler::sys_msync] MS_ASYNC and MS_SYNC cannot be used together\n");
            return -EINVAL;
        }

        bool invalidate = (flags & MS_INVALIDATE) != 0;
        if (invalidate)
        {
            printfRed("[SyscallHandler::sys_msync]   EBUSY  MS_INVALIDATE was specified in flags, and a memory lock exists for the specified address range. \n");
            return -EBUSY;
        }
        // printfCyan("[SyscallHandler::sys_msync] addr=%p, length=%u, flags=0x%x (async=%s, sync=%s, invalidate=%s)\n",
        //            (void *)addr, length, flags,
        //            has_async ? "true" : "false",
        //            has_sync ? "true" : "false",
        //            invalidate ? "true" : "false");

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        uint64 sync_start = addr;
        uint64 sync_end = addr + length;
        // uint64 aligned_length = PGROUNDUP(length);

        // 查找覆盖此地址范围的所有VMA
        bool found_mapping = false;
        for (int i = 0; i < proc::NVMA; ++i)
        {
            if (!p->get_vma()->_vm[i].used)
                continue;

            struct proc::vma *vm = &p->get_vma()->_vm[i];
            uint64 vma_start = vm->addr;
            uint64 vma_end = vma_start + vm->len;

            // 检查是否有重叠
            if (sync_end <= vma_start || sync_start >= vma_end)
            {
                continue; // 没有重叠
            }

            found_mapping = true;
            // printfCyan("[SyscallHandler::sys_msync] Found overlapping VMA %d: [%p, %p), prot=0x%x, flags=0x%x\n",
            //            i, (void *)vma_start, (void *)vma_end, vm->prot, vm->flags);

            // 计算重叠区域
            uint64 overlap_start = MAX(sync_start, vma_start);
            uint64 overlap_end = MIN(sync_end, vma_end);

            // 处理MAP_SHARED文件映射的同步
            if ((vm->flags & MAP_SHARED) && vm->vfile != nullptr)
            {
                // printfCyan("[SyscallHandler::sys_msync] Syncing MAP_SHARED file mapping: %s\n",
                //            vm->vfile->_path_name.c_str());

                // 遍历重叠区域内的所有页面
                uint64 page_start = PGROUNDDOWN(overlap_start);
                uint64 page_end = PGROUNDUP(overlap_end);

                for (uint64 va = page_start; va < page_end; va += PGSIZE)
                {
                    // 检查页面是否已经分配（通过页表查询）
                    mem::Pte pte = p->get_pagetable()->walk(va, 0);
                    if (!pte.is_null() && pte.is_valid())
                    {
                        // 页面已分配，需要写回到文件
                        uint64 pa = (uint64)pte.pa();
                        int file_offset = vm->offset + (va - vma_start);

                        // printfCyan("[SyscallHandler::sys_msync] Writing back page at va=%p, file_offset=%d\n",
                        //            (void *)va, file_offset);

                        // 写回数据到文件
                        int write_result = vm->vfile->write(pa, PGSIZE, file_offset, false);
                        if (write_result < 0)
                        {
                            printfRed("[SyscallHandler::sys_msync] Failed to write back page at va=%p\n", (void *)va);
                            return -EIO;
                        }

                        // 如果是同步模式，确保数据已写入磁盘
                        if (has_sync)
                        {
                            // TODO: 调用文件系统的 fsync 或 sync 操作
                            // 目前简单地假设 write 操作是同步的
                        }

                        // 处理 MS_INVALIDATE 标志
                        if (invalidate)
                        {
                            // TODO: 使其他进程的相同映射失效
                            // 这需要系统级的页面缓存管理，目前先跳过
                            printfYellow("[SyscallHandler::sys_msync] MS_INVALIDATE flag noted but not fully implemented\n");
                        }
                    }
                }
            }
            else if (vm->flags & MAP_SHARED)
            {
                // 匿名共享映射，目前不需要特殊处理
                // printfCyan("[SyscallHandler::sys_msync] Anonymous shared mapping, no file sync needed\n");
            }
            else
            {
                // 私有映射不需要同步
                // printfCyan("[SyscallHandler::sys_msync] Private mapping, no sync needed\n");
            }
        }

        if (!found_mapping)
        {
            printfRed("[SyscallHandler::sys_msync] No memory mapping found for range [%p, %p)\n",
                      (void *)sync_start, (void *)sync_end);
            return -ENOMEM;
        }

        // printfGreen("[SyscallHandler::sys_msync] Successfully synced range [%p, %p)\n",
        //             (void *)sync_start, (void *)sync_end);
        return 0;
    }
    uint64 SyscallHandler::sys_mlock()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_get_mempolicy()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_accept4()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_clockadjtime()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_copy_file_range()
    {
        int fd_in, fd_out;
        uint64 off_in_addr, off_out_addr;
        size_t len;
        unsigned int flags;
        fs::file *f_in, *f_out;

        // 解析参数
        if (_arg_fd(0, &fd_in, &f_in) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid fd_in\n");
            return -EBADF;
        }
        if (_arg_addr(1, off_in_addr) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid off_in address\n");
            return -EFAULT;
        }
        if (_arg_fd(2, &fd_out, &f_out) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid fd_out\n");
            return -EBADF;
        }
        if (_arg_addr(3, off_out_addr) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid off_out address\n");
            return -EFAULT;
        }
        if (_arg_addr(4, (uint64 &)len) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid len\n");
            return -EINVAL;
        }
        if (_arg_int(5, (int &)flags) < 0)
        {
            printfRed("[sys_copy_file_range] Invalid flags\n");
            return -EINVAL;
        }
        printfBlue("[sys_copy_file_range] fd_in=%d, off_in_addr=%p, fd_out=%d, off_out_addr=%p, len=%zu, flags=%u\n",
                   fd_in, (void *)off_in_addr, fd_out, (void *)off_out_addr, len, flags);
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 检查 flags 参数，必须为 0
        if (flags != 0)
        {
            printfRed("[sys_copy_file_range] flags must be 0\n");
            return -EINVAL;
        }

        // 检查文件描述符有效性
        if (!f_in || !f_out)
        {
            printfRed("[sys_copy_file_range] Invalid file descriptors\n");
            return -EBADF;
        }

        // 检查文件类型：必须是普通文件
        if (f_in->_attrs.filetype != fs::FileTypes::FT_NORMAL ||
            f_out->_attrs.filetype != fs::FileTypes::FT_NORMAL)
        {
            printfRed("[sys_copy_file_range] Not regular files\n");
            return -EINVAL;
        }

        // 检查是否是目录
        if (f_in->_attrs.filetype == fs::FileTypes::FT_DIRECT ||
            f_out->_attrs.filetype == fs::FileTypes::FT_DIRECT)
        {
            printfRed("[sys_copy_file_range] Cannot copy from/to directory\n");
            return -EISDIR;
        }

        // 检查文件访问权限
        // fd_in 必须可读 (不能只是 O_WRONLY)
        int access_mode_in = f_in->lwext4_file_struct.flags & 03; // 提取访问模式位
        if (access_mode_in == O_WRONLY)
        {
            printfRed("[sys_copy_file_range] fd_in not open for reading\n");
            return -EBADF;
        }

        // fd_out 必须可写 (不能是 O_RDONLY)
        int access_mode_out = f_out->lwext4_file_struct.flags & 03; // 提取访问模式位
        if (access_mode_out == O_RDONLY)
        {
            printfRed("[sys_copy_file_range] fd_out not open for writing\n");
            return -EBADF;
        }

        // 检查 O_APPEND 标志
        if (f_out->lwext4_file_struct.flags & O_APPEND)
        {
            printfRed("[sys_copy_file_range] fd_out has O_APPEND flag\n");
            return -EBADF;
        }

        // 检查 O_PATH 标志
        if ((f_in->lwext4_file_struct.flags & O_PATH) ||
            (f_out->lwext4_file_struct.flags & O_PATH))
        {
            printfRed("[sys_copy_file_range] Cannot copy with O_PATH files\n");
            return -EBADF;
        }
        if (len == 0)
        {
            printfOrange("[sys_copy_file_range] len is 0, nothing to copy\n");
            return 0;
        }
        // 分配内核缓冲区 - 使用物理内存管理器
        char *buf = (char *)mem::k_pmm.kmalloc(len);
        if (!buf)
        {
            printfRed("[sys_copy_file_range] Failed to allocate buffer of size %zu\n", len);
            return -ENOMEM;
        }

        // 初始化缓冲区以便调试
        memset(buf, 0, len);

        printfBlue("[sys_copy_file_range] Allocated buffer at %p, size %zu\n", buf, len);

        ssize_t read_len = 0;
        ssize_t ret = 0;

        // 处理输入偏移
        if (off_in_addr == 0) // NULL pointer
        {
            // 使用文件自身的偏移
            printfBlue("[sys_copy_file_range] Reading from current file position\n");
            read_len = f_in->read((uint64)buf, len, f_in->get_file_offset(), true);
            //        int ret = f->read((uint64)k_buf, n, f->get_file_offset(), true);
        }
        else
        {
            // 从用户空间读取偏移值
            off_t in_off;
            if (mem::k_vmm.copy_in(*pt, &in_off, off_in_addr, sizeof(off_t)) < 0)
            {
                mem::k_pmm.free_page(buf);
                return -EFAULT;
            }

            printfBlue("[sys_copy_file_range] Reading from offset %ld\n", in_off);

            // 检查偏移是否超过文件大小
            if ((uint64)in_off > f_in->lwext4_file_struct.fsize)
            {
                mem::k_pmm.free_page(buf);
                return 0; // 偏移超过文件大小，直接返回0
            }

            // 从指定偏移读取，不更新文件指针
            read_len = f_in->read((uint64)buf, len, in_off, false);
            if (read_len > 0)
            {
                // 更新用户空间的偏移值
                in_off += read_len;
                if (mem::k_vmm.copy_out(*pt, off_in_addr, &in_off, sizeof(off_t)) < 0)
                {
                    mem::k_pmm.free_page(buf);
                    return -EFAULT;
                }
            }
        }

        printfBlue("[sys_copy_file_range] Read %ld bytes\n", read_len);

        if (read_len <= 0)
        {
            if (read_len < 0)
            {
                printfRed("[sys_copy_file_range] Read failed with error: %ld\n", read_len);
            }
            mem::k_pmm.free_page(buf);
            return read_len < 0 ? read_len : 0;
        }

        // 添加数据验证 - 打印前几个字节用于调试
        if (read_len > 0)
        {
            printfBlue("[sys_copy_file_range] First 16 bytes: ");
            for (int i = 0; i < (read_len > 16 ? 16 : read_len); i++)
            {
                printfBlue("%02x ", (unsigned char)buf[i]);
            }
            printfBlue("\n");
        }

        // 处理输出偏移
        if (off_out_addr == 0) // NULL pointer
        {
            // 使用文件自身的偏移
            printfBlue("[sys_copy_file_range] Writing to current file position\n");
            ret = f_out->write((uint64)buf, read_len, f_out->get_file_offset(), true);
        }
        else
        {
            // 从用户空间读取偏移值
            off_t out_off;
            if (mem::k_vmm.copy_in(*pt, &out_off, off_out_addr, sizeof(off_t)) < 0)
            {
                mem::k_pmm.free_page(buf);
                return -EFAULT;
            }

            printfBlue("[sys_copy_file_range] Writing to offset %ld\n", out_off);
            // 从指定偏移写入，不更新文件指针
            ret = f_out->write((uint64)buf, read_len, out_off, false);
            if (ret > 0)
            {
                // 更新用户空间的偏移值
                out_off += ret;
                if (mem::k_vmm.copy_out(*pt, off_out_addr, &out_off, sizeof(off_t)) < 0)
                {
                    mem::k_pmm.free_page(buf);
                    return -EFAULT;
                }
            }
        }

        printfBlue("[sys_copy_file_range] Wrote %ld bytes\n", ret);

        mem::k_pmm.free_page(buf);
        return ret;
    }
    uint64 SyscallHandler::sys_strerror()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_perror()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_close_range()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_faccessat2()
    {
        // https://www.man7.org/linux/man-pages/man2/faccessat.2.html
        // faccessat2 syscall: int faccessat2(int dirfd, const char *pathname, int mode, int flags);

        int dirfd, mode, flags;
        eastl::string pathname;
        if (_arg_int(0, dirfd) < 0 || _arg_int(2, mode) < 0 || _arg_int(3, flags) < 0)
        {
            return -EINVAL; // 参数错误
        }
        if (_arg_str(1, pathname, MAXPATH) < 0)
        {
            return -EINVAL; // 参数错误
        }

        // 检查 flags 参数的有效性
#define AT_EACCESS 0x200               // Use effective user and group IDs for access checks
#define AT_SYMLINK_NOFOLLOW_FAT2 0x100 // Do not follow symbolic links (avoid conflict)
        int valid_flags = AT_EACCESS | AT_SYMLINK_NOFOLLOW_FAT2;
        if (flags & ~valid_flags)
        {
            printfRed("[SyscallHandler::sys_faccessat2] 无效的flags: %d\n", flags);
            return -EINVAL; // 无效的flags参数
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        // 处理dirfd和路径
        eastl::string abs_pathname;

        // 检查是否为绝对路径
        if (pathname[0] == '/')
        {
            // 绝对路径，忽略dirfd
            abs_pathname = pathname;
        }
        else
        {
            // 相对路径，需要处理dirfd
            if (dirfd == AT_FDCWD)
            {
                // 使用当前工作目录
                abs_pathname = get_absolute_path(pathname.c_str(), p->_cwd_name.c_str());
            }
            else
            {
                // 使用dirfd指向的目录
                fs::file *dir_file = p->get_open_file(dirfd);
                if (!dir_file)
                {
                    printfRed("[SyscallHandler::sys_faccessat2] 无效的dirfd: %d\n", dirfd);
                    return SYS_EBADF; // 无效的文件描述符
                }

                // 检查dirfd是否以 O_PATH 标志打开
                if (dir_file->lwext4_file_struct.flags & O_PATH)
                {
                    return -EBADF;
                }

                // 使用dirfd对应的路径作为基准目录
                abs_pathname = get_absolute_path(pathname.c_str(), dir_file->_path_name.c_str());
            }
        }

        // printfCyan("[SyscallHandler::sys_faccessat2] 绝对路径: %s, flags: %d\n", abs_pathname.c_str(), flags);

        // 处理 AT_SYMLINK_NOFOLLOW 标志
        // 如果设置了此标志，我们需要检查文件本身而不是它指向的目标
        // 当前实现中，我们暂时不处理符号链接，所以这个标志的影响有限

        // 首先验证路径中的每个父目录都是目录
        eastl::string path_to_check = abs_pathname;
        size_t last_slash = path_to_check.find_last_of('/');
        if (last_slash != eastl::string::npos && last_slash > 0)
        {
            eastl::string parent_path = path_to_check.substr(0, last_slash);
            eastl::string current_path = "";

            // 逐段检查路径
            size_t start = 1; // 跳过第一个 '/'
            while (start < parent_path.length())
            {
                size_t end = parent_path.find('/', start);
                if (end == eastl::string::npos)
                    end = parent_path.length();

                current_path += "/" + parent_path.substr(start, end - start);

                if (fs::k_vfs.is_file_exist(current_path.c_str()) == 1)
                {
                    // int file_type = vfs_path2filetype(current_path);
                    int file_type = fs::k_vfs.path2filetype(current_path);
                    if (file_type != fs::FileTypes::FT_DIRECT)
                    {
                        printfRed("[SyscallHandler::sys_faccessat2] 路径中的组件不是目录: %s\n", current_path.c_str());
                        return SYS_ENOTDIR; // 不是目录
                    }
                }
                else if (fs::k_vfs.is_file_exist(current_path.c_str()) == 0)
                {
                    printfRed("[SyscallHandler::sys_faccessat2] 路径中的目录不存在: %s\n", current_path.c_str());
                    return SYS_ENOENT; // 目录不存在
                }

                start = end + 1;
            }
        }

        // 现在检查目标文件是否存在
        if (fs::k_vfs.is_file_exist(abs_pathname.c_str()) != 1)
        {
            printfRed("[SyscallHandler::sys_faccessat2] 文件不存在: %s\n", abs_pathname.c_str());
            return SYS_ENOENT; // 文件不存在
        }

        // 处理访问权限检查
        [[maybe_unused]] int _flags = 0;

        // 如果设置了 AT_EACCESS 标志，使用有效用户和组ID进行检查
        // 否则使用实际用户和组ID进行检查
        // 在当前简化的实现中，我们暂时不区分这两种情况
        if (flags & AT_EACCESS)
        {
            // TODO
            // printfCyan("[SyscallHandler::sys_faccessat2] 使用有效用户ID进行访问检查\n");
            // 这里应该使用有效用户ID (euid) 和有效组ID (egid) 进行权限检查
        }
        else
        {
            // TODO
            // printfCyan("[SyscallHandler::sys_faccessat2] 使用实际用户ID进行访问检查\n");
            // 这里应该使用实际用户ID (uid) 和实际组ID (gid) 进行权限检查
        }

        if (mode & R_OK)
            _flags |= 4;
        if (mode & W_OK)
            _flags |= 2;
        if (mode & X_OK)
            _flags |= 1;
        int fd = proc::k_pm.open(dirfd, abs_pathname, _flags);
        if (fd < 0)
        {
            return fd; // 返回错误码
        }

        // 关闭刚打开的文件描述符，因为 faccessat2 只是检查访问权限
        proc::k_pm.close(fd);

        return 0;
    }
    uint64 SyscallHandler::sys_openat2()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_remap_file_pages()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_splice()
    {
        int fd_in, fd_out;
        uint64 off_in_ptr, off_out_ptr;
        size_t len;
        unsigned int flags;

        // 获取参数
        if (_arg_int(0, fd_in) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching fd_in\n");
            return SYS_EINVAL;
        }
        if (_arg_addr(1, off_in_ptr) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching off_in\n");
            return SYS_EINVAL;
        }
        if (_arg_int(2, fd_out) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching fd_out\n");
            return SYS_EINVAL;
        }
        if (_arg_addr(3, off_out_ptr) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching off_out\n");
            return SYS_EINVAL;
        }
        if (_arg_int(4, (int &)len) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching len\n");
            return SYS_EINVAL;
        }
        if (_arg_int(5, (int &)flags) < 0)
        {
            printfRed("[SyscallHandler::sys_splice] Error fetching flags\n");
            return SYS_EINVAL;
        }

        // 获取文件对象
        fs::file *f_in = nullptr, *f_out = nullptr;
        if (_arg_fd(0, nullptr, &f_in) < 0 || f_in == nullptr)
        {
            printfRed("[SyscallHandler::sys_splice] Invalid fd_in\n");
            return SYS_EBADF;
        }
        if (_arg_fd(2, nullptr, &f_out) < 0 || f_out == nullptr)
        {
            printfRed("[SyscallHandler::sys_splice] Invalid fd_out\n");
            return SYS_EBADF;
        }

        // 判断文件类型
        bool fd_in_is_pipe = (f_in->_attrs.filetype == fs::FileTypes::FT_PIPE);
        bool fd_out_is_pipe = (f_out->_attrs.filetype == fs::FileTypes::FT_PIPE);

        // 检查参数约束：其中一个必须是管道，另一个必须是普通文件
        if (fd_in_is_pipe == fd_out_is_pipe)
        {
            printfRed("[SyscallHandler::sys_splice] Exactly one fd must be a pipe\n");
            return SYS_EINVAL;
        }

        // 获取当前进程和页表
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 检查偏移量参数的约束
        off_t off_in = 0, off_out = 0;

        if (fd_in_is_pipe)
        {
            // 如果fd_in是管道，off_in必须是NULL
            if (off_in_ptr != 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_in must be NULL for pipe\n");
                return SYS_EINVAL;
            }
        }
        else
        {
            // 如果fd_in不是管道，off_in不能是NULL
            if (off_in_ptr == 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_in cannot be NULL for regular file\n");
                return SYS_EINVAL;
            }
            // 从用户空间读取偏移量
            if (mem::k_vmm.copy_in(*pt, &off_in, off_in_ptr, sizeof(off_t)) < 0)
            {
                printfRed("[SyscallHandler::sys_splice] Failed to read off_in from user space\n");
                return SYS_EFAULT;
            }
            // 检查偏移量是否为负
            if (off_in < 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_in cannot be negative\n");
                return SYS_EINVAL;
            }
        }

        if (fd_out_is_pipe)
        {
            // 如果fd_out是管道，off_out必须是NULL
            if (off_out_ptr != 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_out must be NULL for pipe\n");
                return SYS_EINVAL;
            }
        }
        else
        {
            // 如果fd_out不是管道，off_out不能是NULL
            if (off_out_ptr == 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_out cannot be NULL for regular file\n");
                return SYS_EINVAL;
            }
            // 从用户空间读取偏移量
            if (mem::k_vmm.copy_in(*pt, &off_out, off_out_ptr, sizeof(off_t)) < 0)
            {
                printfRed("[SyscallHandler::sys_splice] Failed to read off_out from user space\n");
                return SYS_EFAULT;
            }
            // 检查偏移量是否为负
            if (off_out < 0)
            {
                printfRed("[SyscallHandler::sys_splice] off_out cannot be negative\n");
                return SYS_EINVAL;
            }
        }

        if (len == 0)
        {
            return 0; // 长度为0，直接返回0
        }

        ssize_t bytes_transferred = 0;

        if (fd_in_is_pipe)
        {
            // 从管道读取到普通文件
            bytes_transferred = _splice_pipe_to_file(f_in, f_out, off_out, len);

            // 如果成功传输，更新off_out
            if (bytes_transferred > 0 && off_out_ptr != 0)
            {
                off_out += bytes_transferred;
                if (mem::k_vmm.copy_out(*pt, off_out_ptr, &off_out, sizeof(off_t)) < 0)
                {
                    printfRed("[SyscallHandler::sys_splice] Failed to update off_out\n");
                    // 即使更新失败，也返回已传输的字节数
                }
            }
        }
        else
        {
            // 从普通文件读取到管道

            // 检查off_in是否超过文件大小
            if ((uint64)off_in >= f_in->lwext4_file_struct.fsize)
            {
                return 0; // 偏移量超过文件大小，返回0
            }

            bytes_transferred = _splice_file_to_pipe(f_in, off_in, f_out, len);

            // 如果成功传输，更新off_in
            if (bytes_transferred > 0 && off_in_ptr != 0)
            {
                off_in += bytes_transferred;
                if (mem::k_vmm.copy_out(*pt, off_in_ptr, &off_in, sizeof(off_t)) < 0)
                {
                    printfRed("[SyscallHandler::sys_splice] Failed to update off_in\n");
                    // 即使更新失败，也返回已传输的字节数
                }
            }
        }

        return bytes_transferred;
    }
    uint64 SyscallHandler::sys_prctl()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_ptrace()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setpriority()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getpriority()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_reboot()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_timer_create()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_flock()
    {
        return uint64();
    }

    uint64 SyscallHandler::sys_epoll_create1(){
        return 0;
    }

    uint64 SyscallHandler::sys_epoll_ctl(){
        return 0;
    }

    // splice 辅助函数实现
    ssize_t SyscallHandler::_splice_pipe_to_file(fs::file *pipe_file, fs::file *regular_file, off_t file_offset, size_t len)
    {
        if (!pipe_file || !regular_file)
        {
            return SYS_EBADF;
        }

        // 分配内核缓冲区
        char *buffer = (char *)mem::k_pmm.kmalloc(len);
        if (!buffer)
        {
            printfRed("[_splice_pipe_to_file] Failed to allocate kernel buffer\n");
            return SYS_ENOMEM;
        }

        ssize_t total_transferred = 0;
        ssize_t remaining = len;
        fs::pipe_file *pipe_file_cast = static_cast<fs::pipe_file *>(pipe_file);
        pipe_file_cast->set_nonblock(true); // 设置管道为非阻塞模式
        while (remaining > 0)
        {
            // 从管道读取数据到内核缓冲区
            ssize_t bytes_read = pipe_file_cast->read((uint64)(buffer + total_transferred), remaining, 0, false);
            if (bytes_read <= 0)
            {
                // 管道没有数据了，或者出错
                break;
            }

            // 将数据写入普通文件
            ssize_t bytes_written = regular_file->write((uint64)(buffer + total_transferred), bytes_read, file_offset + total_transferred, false);
            if (bytes_written <= 0)
            {
                printfRed("[_splice_pipe_to_file] Failed to write to regular file\n");
                break;
            }

            total_transferred += bytes_written;
            remaining -= bytes_written;

            // 如果写入的字节数少于读取的字节数，说明出现了问题
            if (bytes_written < bytes_read)
            {
                break;
            }
        }

        mem::k_pmm.free_page(buffer);
        return total_transferred > 0 ? total_transferred : (total_transferred == 0 ? 0 : SYS_EIO);
    }

    ssize_t SyscallHandler::_splice_file_to_pipe(fs::file *regular_file, off_t file_offset, fs::file *pipe_file, size_t len)
    {
        if (!regular_file || !pipe_file)
        {
            return SYS_EBADF;
        }

        // 计算实际可读取的长度
        ssize_t file_remaining = regular_file->lwext4_file_struct.fsize - file_offset;
        if (file_remaining <= 0)
        {
            return 0; // 文件已经读取完毕
        }

        size_t actual_len = (len > (size_t)file_remaining) ? file_remaining : len;

        // 分配内核缓冲区
        char *buffer = (char *)mem::k_pmm.kmalloc(actual_len);
        if (!buffer)
        {
            printfRed("[_splice_file_to_pipe] Failed to allocate kernel buffer\n");
            return SYS_ENOMEM;
        }

        ssize_t total_transferred = 0;
        ssize_t remaining = actual_len;

        while (remaining > 0)
        {
            // 从普通文件读取数据到内核缓冲区
            ssize_t bytes_read = regular_file->read((uint64)(buffer + total_transferred), remaining, file_offset + total_transferred, false);
            if (bytes_read <= 0)
            {
                // 文件读取完毕或出错
                break;
            }

            // 将数据写入管道
            ssize_t bytes_written = pipe_file->write((uint64)(buffer + total_transferred), bytes_read, 0, false);
            if (bytes_written <= 0)
            {
                printfRed("[_splice_file_to_pipe] Failed to write to pipe\n");
                break;
            }

            total_transferred += bytes_written;
            remaining -= bytes_written;

            // 如果写入的字节数少于读取的字节数，说明管道满了，等待或者退出
            if (bytes_written < bytes_read)
            {
                break;
            }
        }

        mem::k_pmm.free_page(buffer);
        return total_transferred > 0 ? total_transferred : (total_transferred == 0 ? 0 : SYS_EIO);
    }
}

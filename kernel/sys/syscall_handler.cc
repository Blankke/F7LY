#include "syscall_handler.hh"
#include "printer.hh"
#include "proc.hh"
#include "proc_manager.hh"
#include "virtual_memory_manager.hh"
#include "klib.hh"
#include "list.hh"
#include "param.h"
#include "sbi.hh"
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
        mem::PageTable* pt = p->get_pagetable();
        if (mem::k_vmm.copy_in(*pt, &out_data, addr, sizeof(out_data)) < 0)
            return -1;
        return 0;
    }

    int SyscallHandler::_fetch_str(uint64 addr, void *buf, uint64 max)
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        int err = mem::k_vmm.copy_str_in(*pt, buf, addr, max);
        if (err < 0)
            return err;
        return strlen((const char *)buf);
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
        uint64 raw_val = _arg_raw(arg_n);
        if (raw_val < (uint64)INT_MIN || raw_val > (uint64)INT_MAX)
        {
            printfRed("[SyscallHandler::_arg_int]arg_n is out of range");
            return -1;
        }
        out_int = (int)raw_val;
        return 0;
    }
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
    int SyscallHandler::_arg_str(int arg_n, char *buf, int max)
    {
        uint64 addr;
        if (_arg_addr(arg_n, addr) < 0)
        {
            printfRed("[SyscallHandler::_arg_str]arg_n is out of range");
            return -1;
        }
        return _fetch_str(addr, buf, max);
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
        return proc::k_pm.fork(); // 调用进程管理器的 fork 函数
    }
    uint64 SyscallHandler::sys_exit()
    {
        TODO("sys_exit");
        printfYellow("sys_exit\n");
        return 0;
    }
    uint64 SyscallHandler::sys_wait()
    {
        TODO("sys_wait");
        printfYellow("sys_wait\n");
        return 0;
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
        TODO("sys_pipe2");
        printfYellow("sys_pipe2\n");
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
        TODO("sys_read");
        printfYellow("sys_read\n");
        return 0;
    }
    uint64 SyscallHandler::sys_kill()
    {
        TODO("sys_kill");
        printfYellow("sys_kill\n");
        return 0;
    }
    uint64 SyscallHandler::sys_execve()
    {
        TODO("sys_execve");
        printfYellow("sys_execve\n");
        return 0;
    }
    uint64 SyscallHandler::sys_fstat()
    {
        TODO("sys_fstat");
        printfYellow("sys_fstat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_statx()
    {
        TODO("sys_statx");
        printfYellow("sys_statx\n");
        return 0;
    }
    uint64 SyscallHandler::sys_chdir()
    {
        TODO("sys_chdir");
        printfYellow("sys_chdir\n");
        return 0;
    }
    uint64 SyscallHandler::sys_dup()
    {
        TODO("sys_dup");
        printfYellow("sys_dup\n");
        return 0;
    }
    uint64 SyscallHandler::sys_sleep()
    {
        TODO("sys_sleep");
        printfYellow("sys_sleep\n");
        return 0;
    }
    uint64 SyscallHandler::sys_uptime()
    {
        TODO("sys_uptime");
        printfYellow("sys_uptime\n");
        return 0;
    }
    uint64 SyscallHandler::sys_openat()
    {
        TODO("sys_openat");
        printfYellow("sys_openat\n");
        return 0;
    }
    uint64 SyscallHandler::sys_write()
    {
        TODO("sys_write");
        printfYellow("sys_write\n");
        return 0;
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
        TODO("sys_close");
        printfYellow("sys_close\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mknod()
    {
        TODO("sys_mkn");
        printfYellow("sys_mkn\n");
        return 0;
    }
    uint64 SyscallHandler::sys_clone()
    {
        TODO("sys_clone");
        printfYellow("sys_clone\n");
        return 0;
    }
    uint64 SyscallHandler::sys_umount2()
    {
        TODO("sys_umount2");
        printfYellow("sys_umount2\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mount()
    {
        TODO("sys_mount");
        printfYellow("sys_mount\n");
        return 0;
    }
    uint64 SyscallHandler::sys_brk()
    {
        TODO("sys_brk");
        printfYellow("sys_brk\n");
        return 0;
    }
    uint64 SyscallHandler::sys_munmap()
    {
        TODO("sys_munmap");
        printfYellow("sys_munmap\n");
        return 0;
    }
    uint64 SyscallHandler::sys_mmap()
    {
        TODO("sys_mmap");
        printfYellow("sys_mmap\n");
        return 0;
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
        TODO("sys_getcwd");
        printfYellow("sys_getcwd\n");
        return 0;
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
    uint64 sys_set_tid_address()
    {
        // TODO
        return 0;
    }
    uint64 sys_getuid()
    {
        // TODO
        return 0;
    }
    uint64 sys_getgid()
    {
        // TODO
        return 0;
    }
    uint64 sys_setgid()
    {
        // TODO
        return 0;
    }
    uint64 sys_setuid()
    {
        // TODO
        return 0;
    }
    uint64 sys_fstatat()
    {
        // TODO
        return 0;
    }
    uint64 sys_exit_group()
    {
        // TODO
        return 0;
    }
    uint64 sys_set_robust_list()
    {
        // TODO
        return 0;
    }
    uint64 sys_gettid()
    {
        // TODO
        return 0;
    }
    uint64 sys_writev()
    {
        // TODO
        return 0;
    }
    uint64 sys_prlimit64()
    {
        // TODO
        return 0;
    }
    uint64 sys_readlinkat()
    {
        // TODO
        return 0;
    }
    uint64 sys_getrandom()
    {
        // TODO
        return 0;
    }
    uint64 sys_clock_gettime()
    {
        // TODO
        return 0;
    }
    uint64 sys_ioctl()
    {
        // TODO
        return 0;
    }
    uint64 sys_syslog()
    {
        // TODO
        return 0;
    }
    uint64 sys_fcntl()
    {
        // TODO
        return 0;
    }
    uint64 sys_faceessat()
    {
        // TODO
        return 0;
    }
    uint64 sys_sysinfo()
    {
        // TODO
        return 0;
    }
    uint64 sys_ppoll()
    {
        // TODO
        return 0;
    }

    uint64 sys_sendfile()
    {
        // TODO
        return 0;
    }
    uint64 sys_readv()
    {
        // TODO
        return 0;
    }
    uint64 sys_geteuid()
    {
        // TODO
        return 0;
    }
    uint64 sys_madvise()
    {
        // TODO
        return 0;
    }
    uint64 sys_mremap()
    {
        // TODO
        return 0;
    }

    uint64 sys_lseek()
    {
        // TODO
        return 0;
    }
    uint64 sys_utimensat()
    {
        // TODO
        return 0;
    }
    uint64 sys_renameat2()
    {
        // TODO
        return 0;
    }

    uint64 sys_clock_nanosleep()
    {
        // TODO
        return 0;
    }
} // namespace syscall

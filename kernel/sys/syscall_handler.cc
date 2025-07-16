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
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/pipe_file.hh"
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
        BIND_SYSCALL(ftruncate); // todo
        BIND_SYSCALL(faccessat); // todo
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
        // ...existing code...    
        // printfCyan("====================debug: syscall_num_list\n");
        // for (uint64 i = 0; i < max_syscall_funcs_num; i++)
        // {
        //     if (_syscall_funcs[i] != nullptr && _syscall_name[i] != nullptr)
        //     {
        //         printfCyan("syscall_num: %d, syscall_name: %s\n", i, _syscall_name[i]);
        //     }
        // }
        printfGreen("[SyscallHandler::init]SyscallHandler initialized with %d syscall functions\n", max_syscall_funcs_num);
    }
    void SyscallHandler::invoke_syscaller()
    {
        proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
        uint64 sys_num = p->get_trapframe()->a7; // 获取系统调用号

        if (!(sys_num == 64) && !(sys_num == 66))
        {
            // printf("---------- start ------------\n");
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
            // if (!(sys_num == 64 && p->_trapframe->a0 == 1) && !(sys_num == 66 && p->_trapframe->a0 == 1))
            if (!(sys_num == 64) && !(sys_num == 66))
                printfCyan("[SyscallHandler::invoke_syscaller]syscall name: %s ret: %p\n", _syscall_name[sys_num], ret);
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
                      "raw_val: %ld, LONG_MIN: %ld, LONG_MAX: %ld\n",
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
        // printf("[SyscallHandler::sys_wait4] waitret: %d\n",
        //    waitret);
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

        static int string_length = 0;
        string_length += strlen(k_buf);
        // printf("[sys_read] read %d characters in total\n", string_length);
        // 添加调试打印，显示读取到的内容
        k_buf[ret] = '\0'; // 确保字符串以null结尾
        // printfCyan("[sys_read] fd=%d, read %d bytes: \"%s\"\n", fd, ret, k_buf);

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
        // printfRed("openat filename %s return [fd] is %d file: %p refcnt: %d\n", path.c_str(), res, p->_ofile[res], p->_ofile[res]->refcnt);
        return res;
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
            return -1;
        }
        if (_arg_addr(1, p) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error fetching address argument\n");
            return -1;
        }
        if (_arg_int(2, n) < 0)
        {
            printfRed("[SyscallHandler::sys_write] Error fetching n argument\n");
            return -1;
        }

        // if (fd > 2)
        //     printfRed("invoke sys_write\n");
        // printf("syscall_write: fd: %d, p: %p, n: %d\n", fd, (void *)p, n);
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
        panic("未实现该系统调用");
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
        panic("未实现该系统调用");
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

        if (mem::k_vmm.copy_str_in(*pt, special, specialaddr, 100) < 0)
            return -1;

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

        // if(dev == "/dev/vda2")
        // {
        //     panic("look in my eyes：你为什么要挂vda2？");
        //     return 0;
        // }

        eastl::string abs_path =  get_absolute_path(mnt.c_str(), p->_cwd_name.c_str()); //< 获取绝对路径

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
        uint64 ret = proc::k_pm.brk(n); // 调用进程管理器的 brk 函数
        // printf("[SyscallHandler::sys_brk] brk to %p, ret: %p\n", (void *)n, (void *)ret);
        return ret;
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
        printfYellow("[SyscallHandler::sys_mmap] addr: %p, map_size: %u, prot: %d, flags: %d, fd: %d, offset: %u\n",
               (void *)addr, map_size, prot, flags, fd, offset);
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
        // printf("[SyscallHandler::sys_gettimeofday] tv: %d.%d\n", tv.tv_sec, tv.tv_usec);

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
        int count = vfs_getdents(f, (struct linux_dirent64 *)sys_getdents64_buf, buf_len);
        mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();
        mem::k_vmm.copy_out(*pt, (uint64)buf_addr, (char *)sys_getdents64_buf, count);
        return count;

        //下面是蒙老师的userspacestream版本，看不懂
        // mem::PageTable *pt = proc::k_pm.get_cur_pcb()->get_pagetable();

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
        return 0;
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
        printf("[SyscallHandler::sys_rt_sigaction] signum: %d, newactaddr: %p, oldactaddr: %p\n",
               signum, (void *)newactaddr, (void *)oldactaddr);

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
        return 0;
    }
    uint64 SyscallHandler::sys_rt_sigreturn()
    {
        proc::ipc::signal::sig_return();
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
        // TODO,这个系统调用关掉了
        int dirfd;
        eastl::string path;
        uint64 kst_addr;
        int flags;
        fs::Kstat kst;

        // 获取参数
        if (_arg_int(0, dirfd) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error fetching dirfd argument\n");
            return -1;
        }

        if (_arg_str(1, path, 256) < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Error fetching path argument\n");
            return -1;
        }

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

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        // 尝试打开文件以获取文件描述符，使用和 sys_openat 相同的方式
        int fd = proc::k_pm.open(dirfd, path, O_RDONLY);
        if (fd < 0)
        {
            printfRed("[SyscallHandler::sys_fstatat] Failed to open file: %s\n", path.c_str());
            return -1;
        }

        // 获取文件状态信息
        if (proc::k_pm.fstat(fd, &kst) < 0)
        {
            proc::k_pm.close(fd);
            printfRed("[SyscallHandler::sys_fstatat] Failed to get file stat\n");
            return -1;
        }

        // 关闭临时打开的文件描述符
        proc::k_pm.close(fd);

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
            nlim = (proc::rlimit64 *)to_vir((ulong)pt->walk_addr(new_limit));
        if (old_limit != 0)
            olim = (proc::rlimit64 *)to_vir((ulong)pt->walk_addr(old_limit));
#endif

        return proc::k_pm.prlimit64(pid, rsrc, nlim, olim);
        ;
    }
    uint64 SyscallHandler::sys_readlinkat()
    {
        panic("未实现");
#ifdef FS_FIX_COMPLETELY
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

        if (path == "/proc/self/exe")
        {
            // TODO,出问题再说
            eastl::string exe_path = proc::k_pm.get_cur_pcb()->_cwd_name + "busybox";
            char *buffer = (char *)exe_path.c_str();
            ret = exe_path.size();
            if (mem::k_vmm.copy_out(*pt, buf, buffer, ret) < 0)
            {
                delete[] buffer;
                return -1;
            }
            // 不要 delete buffer，因为它不是 new 出来的
            return ret;
        }

        if (fd == AT_FDCWD)
            new (&filePath) fs::Path(path, p->_cwd);
        else
            new (&filePath) fs::Path(path, p->get_open_file(fd));

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
        #endif
        return -1; // 未实现
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

        if (addr == 0)
            return -3;

        tmm::timespec tp;
        tmm::SystemClockId cid = (tmm::SystemClockId)clock_id;

        // 调用时间管理器获取时间，传递栈上的对象
        int ret = tmm::k_tm.clock_gettime(cid, &tp);
        if (ret < 0)
            return ret;

        printfYellow("[SyscallHandler::sys_clock_gettime] clock_id: %d, tp: %d.%09d\n", clock_id, tp.tv_sec, tp.tv_nsec);
        // 使用 copy_out 将结果安全地拷贝到用户空间
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        if (mem::k_vmm.copy_out(*pt, addr, &tp, sizeof(tp)) < 0)
        {
            printfRed("[SyscallHandler::sys_clock_gettime] Error copying timespec to user space\n");
            return -4;
        }

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
                return -1;
            return 0;
        }

        printfRed("[SyscallHandler::sys_ioctl] Unsupported ioctl command: 0x%X\n", cmd);
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
        int fd;
        int op;
        ulong arg;
        int retfd = -1;

        if (_arg_fd(0, &fd, &f) < 0)
            return -1;
        if (_arg_int(1, op) < 0)
            return -2;
        // printfYellow("file fd: %d, op: %d\n", fd, op);
        switch (op)
        {
        case F_SETFD:
            if (_arg_addr(2, arg) < 0)
                return -3;
            if (p->_ofile == nullptr)
                return -1;
            if (arg & FD_CLOEXEC)
                p->_ofile->_fl_cloexec[fd] = true;
            else
                p->_ofile->_fl_cloexec[fd] = false;
            return 0;

        case F_DUPFD:
            if (_arg_addr(2, arg) < 0)
                return -3;
            if (p->_ofile == nullptr)
                return -1;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    f->refcnt++;
                    p->_ofile->_fl_cloexec[retfd] = false; // 新的文件描述符默认不设置 CLOEXEC
                    break;
                }
            }
            return retfd;

        case F_DUPFD_CLOEXEC:
            if (_arg_addr(2, arg) < 0)
                return -3;
            if (p->_ofile == nullptr)
                return -1;
            for (int i = (int)arg; i < (int)proc::max_open_files; ++i)
            {
                if ((retfd = proc::k_pm.alloc_fd(p, f, i)) == i)
                {
                    f->refcnt++;
                    p->_ofile->_fl_cloexec[retfd] = true; // 设置 CLOEXEC 标志
                    break;
                }
            }
            return retfd;

        default:
            break;
        }

        return retfd;
    }
    uint64 SyscallHandler::sys_faccessat()
    {
        // panic("未实现");
// #ifdef FS_FIX_COMPLETELY
        int dirfd, mode, flags;
        eastl::string path;
        if (_arg_int(0, dirfd) < 0 || _arg_int(2, mode) < 0 || _arg_int(3, flags) < 0)
        {
            return -1;
        }
        if (_arg_str(1, path, MAXPATH) < 0)
        {
            return -1;
        }

        // fs::dentry *pen;
        // fs::dentry *base_dentry = nullptr; // 保存原始的 dentry 用于路径解析
        // eastl::string pcb_path;
        // eastl::string dirpath;
        // char abs_path[EXT4_PATH_LONG_MAX];
        // if (dirfd == AT_FDCWD)
        // {
        //     //获取CWD的绝对路径
        //     panic("[SyscallHandler::sys_faccessat] AT_FDCWD is not supported yet");
        // }
        // else
        // {
        //     fs::file *ofile = proc::k_pm.get_cur_pcb()->get_open_file(dirfd);
        //     if (ofile == nullptr || ofile->_attrs.filetype != fs::FileTypes::FT_NORMAL)
        //     {
        //         return -1; // 不是普通文件类型
        //     }
        //     get_absolute_path(path.c_str(), proc::k_pm.get_cur_pcb()->_cwd_name.c_str(), abs_path);

        // }

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
        int fd = proc::k_pm.open(dirfd, path, _flags);
        if (fd < 0)
        {
            return -1;
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
            return -1;
        if (_arg_addr(1, iov_ptr) < 0)
            return -2;
        if (_arg_int(2, iovcnt) < 0)
            return -3;

        if (f == nullptr || iovcnt <= 0)
            return -4;

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
            return -5;

        // 从用户空间拷贝iovec数组
        if (mem::k_vmm.copy_in(*pt, vec, iov_ptr, totsize) < 0)
        {
            delete[] vec;
            return -6;
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
                return -7;
            }
            int ret = f->read((uint64)k_buf, vec[i].iov_len, f->get_file_offset(), true);
            if (ret < 0)
            {
                delete[] k_buf;
                delete[] vec;
                return -8;
            }
            if (mem::k_vmm.copy_out(*pt, (uint64)vec[i].iov_base, k_buf, ret) < 0)
            {
                delete[] k_buf;
                delete[] vec;
                return -9;
            }
            nread += ret;
            delete[] k_buf;
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
        panic("未实现该系统调用");
    }

    uint64 SyscallHandler::sys_lseek()
    {
        int fd;
        long offset;
        int whence;

        if (_arg_int(0, fd) < 0)
            return -1;

        if (_arg_long(1, offset) < 0)
            return -1;

        if (_arg_int(2, whence) < 0)
            return -1;

        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
        fs::file *f = cur_proc->get_open_file(fd);

        if (f == nullptr)
            return -1;

        return f->lseek(offset, whence);
    }
    uint64 SyscallHandler::sys_utimensat()
    {
        //TODO: 这个完全是骗的
        // panic("未实现");
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
        pathname =  get_absolute_path(pathname.c_str(), cur_proc->_cwd_name.c_str());
        if (is_file_exist(pathname.c_str()) != 1)
            return -ENOENT;

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
        if (mem::k_vmm.copy_str_in(*pt, old_path, old_path_addr, MAXPATH) < 0)
            return -1;
        if (mem::k_vmm.copy_str_in(*pt, new_path, new_path_addr, MAXPATH) < 0)
            return -1;

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
            return -EINVAL;
        }

        // 仅示例支持 CLOCK_REALTIME 与 CLOCK_MONOTONIC
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
            return -EINVAL;

        // 从用户空间复制 timespec
        tmm::timespec req_ts;
        if (req_addr != 0)
        {
            if (mem::k_vmm.copy_in(*proc::k_pm.get_cur_pcb()->get_pagetable(),
                                   &req_ts, req_addr, sizeof(req_ts)) < 0)
                return -EFAULT;
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
            //     return -EINTR;
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
        // 尝试获取两个参数
        if (_arg_addr(0, path_addr) < 0 || _arg_addr(1, buf_addr) < 0)
            return -1;
        statfs st;
        // 根据需求填写字段
        st.f_type = 0x2011BAB0;
        st.f_bsize = PGSIZE; // 假设块大小
        st.f_blocks = 1L << 27;
        st.f_bfree = 1L << 26;
        st.f_bavail = 1L << 20;
        st.f_files = 1L << 10;
        st.f_ffree = 1L << 9;
        st.f_namelen = 1L << 8;
        st.f_frsize = 1L << 9;
        st.f_flags = 1L << 1;

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();
        // 将结果写回用户态
        if (mem::k_vmm.copy_out(*pt, buf_addr, &st, sizeof(st)) < 0)
            return -1;

        return 0;
    }
    uint64 SyscallHandler::sys_ftruncate()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_pread64()
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
        long rc = f->read((ulong)kbuf, count, f->get_file_offset(), true);
        if (rc < 0)
        {
            delete[] kbuf;
            f->lseek(old_off, SEEK_SET);
            return rc;
        }

        if (mem::k_vmm.copy_out(*p->get_pagetable(), buf, kbuf, rc) < 0)
        {
            delete[] kbuf;
            f->lseek(old_off, SEEK_SET);
            return -1;
        }

        f->lseek(old_off, SEEK_SET);
        delete[] kbuf;
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
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sync()
    {
        return 0; // copy from 唐老师
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fsync()
    {
        return 0; // copy from 唐老师
        panic("未实现该系统调用");
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
            if (mem::k_vmm.copy_in(proc::k_pm.get_cur_pcb()->_pt, (char *)&timeout, timeout_addr, sizeof(timeout)) < 0)
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
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sched_getaffinity()
    {
        // return 0;
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setpgid()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getpgid()
    {
        panic("未实现该系统调用");
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
            return -EINVAL;
        }

        // 将结果拷贝到用户空间
        if (mem::k_vmm.copy_out(*pt, usage_addr, &ret, sizeof(ret)) < 0)
        {
            printfRed("[SyscallHandler::sys_getrusage] Error copying rusage to user space\n");
            return -EFAULT;
        }

        return 0;
    }
    uint64 SyscallHandler::sys_getegid()
    {
        return 1;
    }
    uint64 SyscallHandler::sys_shmget()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_shmctl()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_shmat()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_socket()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_socketpair()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_bind()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_listen()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_accept()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_connect()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getsockname()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getpeername()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sendto()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_recvfrom()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_setsockopt()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getsockopt()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_shutdown_socket()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_sendmsg()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_mprotect()
    {
        // printfRed("[SyscallHandler::sys_mprotect] 未实现该系统调用\n");
        // return 0;
        uint64 addr, len;
        int prot;
        if (_arg_addr(0, addr) < 0)
            return -1;
        if (_arg_addr(1, len) < 0)
            return -1;
        if (_arg_int(2, prot) < 0)
            return -1;

        int perm;

        if (prot & PROT_READ)
        {
            perm |= PTE_R;
        }
        if (prot & PROT_WRITE)
        {
            perm |= PTE_W;
        }
        if (prot & PROT_EXEC)
        {
            perm |= PTE_X;
        }
        if (mem::k_vmm.protectpages(*proc::k_pm.get_cur_pcb()->get_pagetable(), addr, len, perm) < 0)
        {
            return -1;
        }
        return 0;
    }
    uint64 SyscallHandler::sys_membarrier()
    {
        return 0;
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_clone3()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_poweroff()
    {
        panic("未实现该系统调用");
    }

        //================================== rocket syscalls ===================================
    uint64 SyscallHandler::sys_fgetxattr()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_mknodat()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_symlinkat()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fstatfs()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_truncate()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fallocate()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchdir()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_chroot()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchmod()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchmodat()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchownat()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_fchown()
    {
        panic("未实现该系统调用");
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
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_getresuid()
    {
        panic("未实现该系统调用");
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
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_adjtimex()
    {
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_shmdt()
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
        panic("未实现该系统调用");
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
        panic("未实现该系统调用");
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
        panic("未实现该系统调用");
    }
    uint64 SyscallHandler::sys_openat2()
    {
        panic("未实现该系统调用");
    }
}

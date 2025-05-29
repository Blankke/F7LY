#pragma once
#include "types.hh"
#include "syscall_defs.hh"
#include "printer.hh"
#include "fs/vfs/file/file.hh"
namespace syscall
{
    constexpr uint max_syscall_funcs_num = 2048;
    constexpr uint max_path_len = 128;
    constexpr uint max_arg_num = 32;

    class SyscallHandler
    {
        // 使用了这个类的成员函数指针
        using SyscallFunc = uint64 (SyscallHandler::*)(); // 定义一个函数指针类型 SyscallFunc
    private:
        SyscallFunc _syscall_funcs[max_syscall_funcs_num]; // 存储系统调用函数的指针数组
        const char *_syscall_name[max_syscall_funcs_num];  // 存储系统调用名称的指针数组
        uint64_t _default_syscall_impl();                  // 默认的系统调用实现
    public:
        void init();             // 使用构造函数进行init
        void invoke_syscaller(); // 调用系统调用
        SyscallHandler()
        {
        }

    private:
        int _fetch_addr(uint64 addr, uint64 &out_data);
        int _fetch_str(uint64 addr, void *buf, uint64 max);
        uint64 _arg_raw(int arg_n);

        int _arg_int(int arg_n, int &out_int);
        int _arg_addr(int arg_n, uint64 &out_addr);
        int _arg_str(int arg_n, char *buf, int max);
        int _arg_fd(int arg_n, int *out_fd, fs::file **out_f);

    private: // ================ syscall functions ================
        uint64 sys_exec();
        uint64 sys_fork();
        uint64 sys_exit();
        uint64 sys_wait();
        uint64 sys_wait4();
        uint64 sys_getppid();
        uint64 sys_getpid();
        uint64 sys_pipe2();
        uint64 sys_dup3();
        uint64 sys_read();
        uint64 sys_kill();
        uint64 sys_execve();
        uint64 sys_fstat();
        uint64 sys_statx();
        uint64 sys_chdir();
        uint64 sys_dup();
        uint64 sys_sleep();
        uint64 sys_uptime();
        uint64 sys_openat();
        uint64 sys_write();
        uint64 sys_unlinkat();
        uint64 sys_linkat();
        uint64 sys_mkdirat();
        uint64 sys_close();
        uint64 sys_mknod();
        uint64 sys_clone();
        uint64 sys_umount2();
        uint64 sys_mount();
        uint64 sys_brk();
        uint64 sys_munmap();
        uint64 sys_mmap();
        uint64 sys_times();
        uint64 sys_uname();
        uint64 sys_sched_yield();
        uint64 sys_gettimeofday();
        uint64 sys_nanosleep();
        uint64 sys_getcwd();
        uint64 sys_getdents64();
        uint64 sys_shutdown();
        // For signal
        uint64 sys_kill_signal();
        uint64 sys_tkill();
        uint64 sys_tgkill();
        uint64 sys_rt_sigaction();
        uint64 sys_rt_sigprocmask();
        uint64 sys_rt_sigtimedwait();
        uint64 sys_rt_sigreturn();

        // busybox
        uint64 sys_set_tid_address();
        uint64 sys_getuid();
        uint64 sys_getgid();
        uint64 sys_setgid();
        uint64 sys_setuid();
        uint64 sys_fstatat();
        uint64 sys_exit_group();
        uint64 sys_set_robust_list();
        uint64 sys_gettid();
        uint64 sys_writev();
        uint64 sys_prlimit64();
        uint64 sys_readlinkat();
        uint64 sys_getrandom();
        uint64 sys_clock_gettime();
        uint64 sys_ioctl();
        uint64 sys_syslog();
        uint64 sys_fcntl();
        uint64 sys_faceessat();
        uint64 sys_sysinfo();
        uint64 sys_ppoll();
        uint64 sys_utimensat();
        uint64 sys_sendfile();
        uint64 sys_geteuid();
        uint64 sys_madvise();
        uint64 sys_mremap();
        uint64 sys_lseek();
        uint64 sys_renameat2();
        uint64 sys_readv();
        uint64 sys_clock_nanosleep();
    };

    extern SyscallHandler k_syscall_handler;

}
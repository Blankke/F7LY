#pragma once
#include "types.hh"
#include "syscall_defs.hh"
#include "printer.hh"
#include "fs/vfs/file/file.hh"
#include "fs/vfs/file.hh"
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
        int _fetch_str(uint64 addr, eastl::string&buf, uint64 max);
        uint64 _arg_raw(int arg_n);

        int _arg_int(int arg_n, int &out_int);
        int _arg_long(int arg_n, long &out_int);
        int _arg_addr(int arg_n, uint64 &out_addr);
        int _arg_str(int arg_n, eastl::string &buf, int max);
        int _arg_fd(int arg_n, int *out_fd, fs::file **out_f);
        int  argfd(int n, int *pfd, struct file **pf);
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
        uint64 sys_fcntl();
        uint64 sys_faccessat();
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

        // 补全的 todo 接口
        uint64 sys_syslog();
        uint64 sys_statfs();
        uint64 sys_ftruncate();
        uint64 sys_pread64();
        uint64 sys_pwrite64();
        uint64 sys_pselect6();
        uint64 sys_sync();
        uint64 sys_fsync();
        uint64 sys_futex();
        uint64 sys_get_robust_list();
        uint64 sys_setitimer();
        uint64 sys_sched_getaffinity();
        uint64 sys_setpgid();
        uint64 sys_getpgid();
        uint64 sys_setsid();
        uint64 sys_getrusage();
        uint64 sys_getegid();
        uint64 sys_shmget();
        uint64 sys_shmctl();
        uint64 sys_shmat();
        uint64 sys_socket();
        uint64 sys_socketpair();
        uint64 sys_bind();
        uint64 sys_listen();
        uint64 sys_accept();
        uint64 sys_connect();
        uint64 sys_getsockname();
        uint64 sys_getpeername();
        uint64 sys_sendto();
        uint64 sys_recvfrom();
        uint64 sys_setsockopt();
        uint64 sys_getsockopt();
        uint64 sys_shutdown_socket();
        uint64 sys_sendmsg();
        uint64 sys_mprotect();
        uint64 sys_membarrier();
        uint64 sys_clone3();
        uint64 sys_poweroff();


        //rocket
        uint64 sys_fgetxattr();
        uint64 sys_mknodat();
        uint64 sys_symlink();
        uint64 sys_symlinkat();
        uint64 sys_fstatfs();
        uint64 sys_truncate();
        uint64 sys_fallocate();
        uint64 sys_fchdir();
        uint64 sys_chroot();
        uint64 sys_fchmod();
        uint64 sys_fchmodat();
        uint64 sys_fchownat();
        uint64 sys_fchown();
        uint64 sys_preadv();
        uint64 sys_pwritev();
        uint64 sys_sync_file_range();
        uint64 sys_acct();
        uint64 sys_clock_settime();
        uint64 sys_clock_getres();
        uint64 sys_sched_setscheduler();
        uint64 sys_sched_getscheduler();
        uint64 sys_sched_getparam();
        uint64 sys_sched_setaffinity();
        uint64 sys_sigaltstack();
        uint64 sys_rt_sigsuspend();
        uint64 sys_rt_sigpending();
        uint64 sys_rt_sigqueueinfo();
        uint64 sys_setregrid();
        uint64 sys_setreuid();
        uint64 sys_setresuid();
        uint64 sys_getresuid();
        uint64 sys_setresgid();
        uint64 sys_getresgid();
        uint64 sys_setfsuid();
        uint64 sys_setfsgid();
        uint64 sys_getgroups();
        uint64 sys_setgroups();
        uint64 sys_sethostname();
        uint64 sys_setdomainname();
        uint64 sys_umask();
        uint64 sys_adjtimex();
        uint64 sys_shmdt();
        uint64 sys_recvmsg();
        uint64 sys_fadvise64();
        uint64 sys_msync();
        uint64 sys_mlock();
        uint64 sys_get_mempolicy();
        uint64 sys_accept4();
        uint64 sys_clockadjtime();
        uint64 sys_copy_file_range();
        uint64 sys_strerror();
        uint64 sys_perror();
        uint64 sys_close_range();
        uint64 sys_openat2();
        uint64 sys_faccessat2();
    };

    extern SyscallHandler k_syscall_handler;

}
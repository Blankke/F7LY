#pragma once
namespace syscall
{

    enum SysNum
    {
        SYS_fork = 1,
        SYS_wait = 3,
        SYS_kill = 6,
        SYS_sleep = 13,
        SYS_uptime = 14,
        SYS_mknod = 16,
        SYS_getcwd = 17,
        SYS_shutdown = 19,
        SYS_dup = 23,
        SYS_dup3 = 24,
        SYS_fcntl = 25,
        SYS_ioctl = 29,
        SYS_mkdirat = 34,
        SYS_unlinkat = 35,
        SYS_linkat = 37,
        SYS_umount2 = 39,
        SYS_mount = 40,
        SYS_statfs = 43, //todo
        SYS_ftruncate = 46, //todo
        SYS_faccessat = 48, //todo
        SYS_chdir = 49,
        SYS_exec = 55,
        SYS_openat = 56,
        SYS_close = 57,
        SYS_pipe2 = 59,
        SYS_getdents64 = 61,
        SYS_lseek = 62,
        SYS_read = 63,
        SYS_write = 64,
        SYS_readv = 65,
        SYS_writev = 66,
        SYS_pread64 = 67, //todo
        SYS_pwrite64 = 68, //todo
        SYS_sendfile = 71,
        SYS_pselect6 = 72, //todo
        SYS_ppoll = 73,
        SYS_readlinkat = 78,
        SYS_fstatat = 79,
        SYS_fstat = 80,
        SYS_sync = 81, //todo
        SYS_fsync = 82, //todo
        SYS_utimensat = 88,
        SYS_exit = 93,
        SYS_exit_group = 94,
        SYS_set_tid_address = 96,
        SYS_futex = 98, //todo
        SYS_set_robust_list = 99,
        SYS_get_robust_list = 100, //todo
        SYS_nanosleep = 101,
        SYS_setitimer = 103, //todo
        SYS_clock_gettime = 113,
        SYS_clock_nanosleep = 115,
        SYS_syslog = 116,
        SYS_sched_getaffinity = 123, //todo
        SYS_sched_yield = 124,
        SYS_kill_signal = 129,
        SYS_tkill = 130,
        SYS_tgkill = 131,
        SYS_rt_sigaction = 134,
        SYS_rt_sigprocmask = 135,
        SYS_rt_sigtimedwait = 137,
        SYS_rt_sigreturn = 139,
        SYS_setgid = 144,
        SYS_setuid = 146,
        SYS_times = 153,
        SYS_setpgid = 154, //todo
        SYS_getpgid = 155, //todo
        SYS_setsid = 157, //todo
        SYS_uname = 160,
        SYS_getrusage = 165, //todo
        SYS_gettimeofday = 169,
        SYS_getpid = 172,
        SYS_getppid = 173,
        SYS_getuid = 174,
        SYS_geteuid = 175,
        SYS_getgid = 176,
        SYS_getegid = 177, //todo
        SYS_gettid = 178,
        SYS_sysinfo = 179,
        SYS_shmget = 194, //todo
        SYS_shmctl = 195, //todo
        SYS_shmat = 196, //todo
        SYS_socket = 198, //todo
        SYS_socketpair = 199, //todo
        SYS_bind = 200, //todo
        SYS_listen = 201, //todo
        SYS_accept = 202, //todo
        SYS_connect = 203, //todo
        SYS_getsockname = 204, //todo
        SYS_getpeername = 205, //todo
        SYS_sendto = 206, //todo
        SYS_recvfrom = 207, //todo
        SYS_setsockopt = 208, //todo
        SYS_getsockopt = 209, //todo
        SYS_sendmsg = 211, //todo
        SYS_brk = 214,
        SYS_munmap = 215,
        SYS_mremap = 216,
        SYS_clone = 220,
        SYS_execve = 221,
        SYS_mmap = 222,
        SYS_mprotect = 226, //todo
        SYS_madvise = 233,
        SYS_membarrier = 283, //todo
        SYS_wait4 = 260,
        SYS_prlimit64 = 261,
        SYS_renameat2 = 276,
        SYS_getrandom = 278,
        SYS_statx = 291,
        SYS_clone3 = 435, //todo
        SYS_poweroff = 2025 //todo
    };

    enum SYS_clone
    {
        SIGCHILD = 17
    };
}
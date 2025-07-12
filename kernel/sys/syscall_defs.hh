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
        SYS_statfs = 43,    // todo
        SYS_ftruncate = 46, // todo
        SYS_faccessat = 48, // todo
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
        SYS_pread64 = 67,  // todo
        SYS_pwrite64 = 68, // todo
        SYS_sendfile = 71,
        SYS_pselect6 = 72, // todo
        SYS_ppoll = 73,
        SYS_readlinkat = 78,
        SYS_fstatat = 79,
        SYS_fstat = 80,
        SYS_sync = 81,  // todo
        SYS_fsync = 82, // todo
        SYS_utimensat = 88,
        SYS_exit = 93,
        SYS_exit_group = 94,
        SYS_set_tid_address = 96,
        SYS_futex = 98, // todo
        SYS_set_robust_list = 99,
        SYS_get_robust_list = 100, // todo
        SYS_nanosleep = 101,
        SYS_setitimer = 103, // todo
        SYS_clock_gettime = 113,
        SYS_clock_nanosleep = 115,
        SYS_syslog = 116,
        SYS_sched_getaffinity = 123, // todo
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
        SYS_setpgid = 154, // todo
        SYS_getpgid = 155, // todo
        SYS_setsid = 157,  // todo
        SYS_uname = 160,
        SYS_getrusage = 165, // todo
        SYS_gettimeofday = 169,
        SYS_getpid = 172,
        SYS_getppid = 173,
        SYS_getuid = 174,
        SYS_geteuid = 175,
        SYS_getgid = 176,
        SYS_getegid = 177, // todo
        SYS_gettid = 178,
        SYS_sysinfo = 179,
        SYS_shmget = 194,      // todo
        SYS_shmctl = 195,      // todo
        SYS_shmat = 196,       // todo
        SYS_socket = 198,      // todo
        SYS_socketpair = 199,  // todo
        SYS_bind = 200,        // todo
        SYS_listen = 201,      // todo
        SYS_accept = 202,      // todo
        SYS_connect = 203,     // todo
        SYS_getsockname = 204, // todo
        SYS_getpeername = 205, // todo
        SYS_sendto = 206,      // todo
        SYS_recvfrom = 207,    // todo
        SYS_setsockopt = 208,  // todo
        SYS_getsockopt = 209,  // todo
        SYS_sendmsg = 211,     // todo
        SYS_brk = 214,
        SYS_munmap = 215,
        SYS_mremap = 216,
        SYS_clone = 220,
        SYS_execve = 221,
        SYS_mmap = 222,
        SYS_mprotect = 226, // todo
        SYS_madvise = 233,
        SYS_membarrier = 283, // todo
        SYS_wait4 = 260,
        SYS_prlimit64 = 261,
        SYS_renameat2 = 276,
        SYS_getrandom = 278,
        SYS_statx = 291,
        SYS_clone3 = 435,   // todo
        SYS_poweroff = 2025 // todo
    };

    enum SYS_clone
    {
                // Cloning flags as enum class for type safety
        CSIGNAL             = 0x000000ff, // Signal mask to be sent at exit.
        CLONE_VM            = 0x00000100, // Set if VM shared between processes.
        CLONE_FS            = 0x00000200, // Set if fs info shared between processes.
        CLONE_FILES         = 0x00000400, // Set if open files shared between processes.
        CLONE_SIGHAND       = 0x00000800, // Set if signal handlers shared.
        CLONE_PIDFD         = 0x00001000, // Set if a pidfd should be placed in parent.
        CLONE_PTRACE        = 0x00002000, // Set if tracing continues on the child.
        CLONE_VFORK         = 0x00004000, // Set if the parent wants the child to wake it up on mm_release.
        CLONE_PARENT        = 0x00008000, // Set if we want to have the same parent as the cloner.
        CLONE_THREAD        = 0x00010000, // Set to add to same thread group.
        CLONE_NEWNS         = 0x00020000, // Set to create new namespace.
        CLONE_SYSVSEM       = 0x00040000, // Set to shared SVID SEM_UNDO semantics.
        CLONE_SETTLS        = 0x00080000, // Set TLS info.
        CLONE_PARENT_SETTID = 0x00100000, // Store TID in userlevel buffer before MM copy.
        CLONE_CHILD_CLEARTID= 0x00200000, // Register exit futex and memory location to clear.
        CLONE_DETACHED      = 0x00400000, // Create clone detached.
        CLONE_UNTRACED      = 0x00800000, // Set if the tracing process can't force CLONE_PTRACE on this clone.
        CLONE_CHILD_SETTID  = 0x01000000, // Store TID in userlevel buffer in the child.
        CLONE_NEWCGROUP     = 0x02000000, // New cgroup namespace.
        CLONE_NEWUTS        = 0x04000000, // New utsname group.
        CLONE_NEWIPC        = 0x08000000, // New ipcs.
        CLONE_NEWUSER       = 0x10000000, // New user namespace.
        CLONE_NEWPID        = 0x20000000, // New pid namespace.
        CLONE_NEWNET        = 0x40000000, // New network namespace.
        CLONE_IO            = 0x80000000, // Clone I/O context.
        CLONE_NEWTIME       = 0x00000080  // New time namespace
    };
    enum SYS_wait
    {
        /* Bits in the third argument to `waitpid'.  */
        WNOHANG = 1,  /* Don't block waiting.  */
        WUNTRACED = 2 /* Report status of stopped children.  */
    };

    enum Errno
    {
        /// 操作不允许（无权限）
        EPERM = -1,
        /// 文件或目录不存在
        ENOENT = -2,
        /// 进程不存在
        ESRCH = -3,
        /// 系统调用被信号中断
        EINTR = -4,
        /// 输入/输出错误（底层硬件或设备故障）
        EIO = -5,
        /// 设备或地址不存在
        ENXIO = -6,
        /// 参数列表过长（如 execve 的参数）
        E2BIG = -7,
        /// 可执行文件格式错误
        ENOEXEC = -8,
        /// 错误的文件描述符（未打开或无效）
        EBADF = -9,
        /// 无子进程（如 waitpid 无目标）
        ECHILD = -10,
        /// 资源暂时不可用（非阻塞操作未就绪）
        /// 等同于 EWOULDBLOCK（通常用于非阻塞 I/O）
        EAGAIN = -11,
        /// 内存不足
        ENOMEM = -12,
        /// 权限不足（文件访问被拒绝）
        EACCES = -13,
        /// 错误的地址（用户空间指针无效）
        EFAULT = -14,
        /// 需要块设备（如对字符设备执行块操作）
        ENOTBLK = -15,
        /// 设备或资源忙（如文件被锁定）
        EBUSY = -16,
        /// 文件已存在（如创建已存在的文件）
        EEXIST = -17,
        /// 跨设备链接（不允许跨文件系统硬链接）
        EXDEV = -18,
        /// 设备不存在
        ENODEV = -19,
        /// 不是目录（期望目录但提供的是文件）
        ENOTDIR = -20,
        /// 是目录（期望文件但提供的是目录）
        EISDIR = -21,
        /// 无效参数（如错误的标志值）
        EINVAL = -22,
        /// 系统文件表溢出（全局文件描述符耗尽）
        ENFILE = -23,
        /// 进程打开文件数超出限制
        EMFILE = -24,
        /// 不是终端设备（如对非终端调用 ioctl）
        ENOTTY = -25,
        /// 文本文件忙（如正在执行的共享库被修改）
        ETXTBSY = -26,
        /// 文件过大（超出文件大小限制）
        EFBIG = -27,
        /// 设备空间不足（如磁盘写满）
        ENOSPC = -28,
        /// 非法寻址（如对管道调用 lseek）
        ESPIPE = -29,
        /// 只读文件系统（尝试修改只读挂载的文件系统）
        EROFS = -30,
        /// 链接数过多（文件系统限制）
        EMLINK = -31,
        /// 管道破裂（写入无读取端的管道）
        EPIPE = -32,
        /// 数学参数超出函数定义域
        EDOM = -33,
        /// 数学结果不可表示（如溢出）
        ERANGE = -34,
        /// 资源死锁可能发生（如线程锁顺序问题）
        EDEADLK = -35,
        /// 文件名过长（超出文件系统限制）
        ENAMETOOLONG = -36,
        /// 无可用记录锁（文件锁资源耗尽）
        ENOLCK = -37,
        /// 无效的系统调用号（如不存在的 syscall）
        ENOSYS = -38,
        /// 目录非空（如删除非空目录）
        ENOTEMPTY = -39,
        /// 符号链接嵌套过深（可能形成环路）
        ELOOP = -40,
        /// 对非套接字执行套接字操作
        ENOTSOCK = -88,
        /// 发送信息超过一次message最大内容
        EMSGSIZE = -90,
        ENOPROTOOPT = -92,
        /// EPROTONOSUPPORT表示不支持所选的套接字协议
        EPROTONOSUPPORT = -93,
        /// 操作不支持（如对普通文件调用套接字操作）
        EOPNOTSUPP = -95,
        // address family 不支持
        EAFNOSUPPORT = -97,
        /// 套接字地址已在使用中（如端口被占用）
        EADDRINUSE = -98,
        /// 地址不可用（如绑定到不存在的 IP）
        EADDRNOTAVAIL = -99,
        ECONNABORTED = -103,
        /// 连接被重置（对端强制关闭）
        ECONNRESET = -104,
        /// 传输端点已连接（如重复调用 connect）
        EISCONN = -106,
        /// 套接字未连接（如未 connect 就 send）
        ENOTCONN = -107,
        /// 操作超时（如网络请求未在指定时间内响应）
        ETIMEDOUT = -110,
        /// 连接被拒绝（对端无监听服务）
        ECONNREFUSED = -111,
        /// 套接字为非阻塞模式且连接无法立即完成
        /// （通常需要配合 select/poll 检查可写性）
        EINPROGRESS = -115,
        /// 内核自动重启系统调用
        ERESTARTSYS = -512,
    };
}
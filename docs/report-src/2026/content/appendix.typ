= 附录

// 设置表格可跨页
#show figure: set block(breakable: true)

== 附录A　PCB 字段总表 <sec:appendix-pcb-fields>

`proc::Pcb` 是 F7LY 用来承载进程与线程全部内核态元数据的统一对象。系统中的所有任务都以 PCB 形式存在于全局进程池中，由调度、信号、文件系统和地址空间等子系统共同引用。

#figure(
  table(
    columns: (4cm, 10cm),
    align: (left, left),
    table.header(
      [*字段名*], [*含义*]
    ),
    table.cell(colspan: 2, align: center, [*标识与状态*]),
    [`_lock`], [PCB 自旋锁，保护并发访问],
    [`_parent`], [父进程 PCB 指针，与 `_ppid` 互补],
    [`_pid, _tid, _tgid, _ppid, _pgid`], [进程号、线程号、线程组号、父进程号、进程组号],
    [`_global_id`], [进程池槽位索引，`alloc_proc()` 分配时确定],
    [`_sid`], [会话 ID，`setsid()` 设置],
    [`_uid / _euid / _suid / _fsuid`], [四组用户 ID（real / effective / saved / filesystem）],
    [`_gid / _egid / _sgid / _fsgid`], [四组组 ID],
    [`_cap_*[2]`], [Linux capability 位图（effective / permitted / inheritable / ambient / bounding），覆盖 41 种能力],
    table.cell(colspan: 2, align: center, [*进程状态*]),
    [`_state`], [进程状态：`UNUSED → USED → RUNNABLE ↔ RUNNING → SLEEPING / ZOMBIE / STOPPED`],
    [`_chan`], [睡眠等待通道，`sleep()` 设置、`wakeup()` 匹配],
    [`_killed, _exiting`], [终止标志；退出清理中标记（禁止 timer 抢占）],
    [`_xstate`], [退出码，父进程通过 `wait4` / `waitid` 获取],
    [`_parent_exit_signal`], [退出时发往父进程的信号，默认 SIGCHLD，线程创建时为 0],
    table.cell(colspan: 2, align: center, [*地址空间与文件*]),
    [`_memory_manager`], [`ProcessMemoryManager*`，封装页表、VMA、堆、mmap 区域。`CLONE_VM` 时共享（引用计数），fork 时深拷贝],
    [`_kstack`], [内核栈虚拟地址，由 `_global_id` 换算，通常 2 页 + 1 页 guard page],
    [`_trapframe`], [`TrapFrame*`，用户态寄存器保存区（一页物理内存），陷入内核时保存 GPR 等现场],
    [`_context`], [`Context`，内核态上下文（ra / sp / callee-saved 寄存器），`swtch()` 在此保存/恢复调度现场],
    [`_ofile`], [文件描述符表（`ofile*`），容量 1024。`CLONE_FILES` 时共享并增加引用计数],
    [`_cwd, _root_name`], [当前工作目录；chroot 根目录],
    [`_umask`], [文件创建权限掩码],
    [`_personality`], [Linux personality(2) 状态],
    table.cell(colspan: 2, align: center, [*信号与同步*]),
    [`_sigactions`], [`sighand_struct*`，sigaction 表，`CLONE_SIGHAND` 时共享。exec 时非 SIG_IGN 的 handler 重置为默认],
    [`_sigmask`], [`uint64`，信号屏蔽掩码（64-bit 位图），`sigprocmask` 修改],
    [`_signal`], [`uint64`，待处理信号掩码，`add_signal()` 设置，`clear_signal()` 清除],
    [`_siginfo_mask, _queued_siginfo[65]`], [附带 siginfo 的排队信息，每个 signal 保留最后一份],
    [`sig_frame`], [`signal_frame*`，信号栈帧链表头。`do_handle()` 压入，`sig_return()` 弹出并恢复 TrapFrame],
    [`_alt_stack, _on_sigstack`], [备用信号栈（`sigaltstack` 设置）及当前是否在其上执行],
    [`_futex_addr`], [futex 等待的用户态地址（调试用）],
    [`_futex_key`], [futex 匹配键，按用户态地址映射到的物理地址计算，跨进程共享映射可正确匹配],
    [`_vfork_parent`], [`CLONE_VFORK` 子进程释放地址空间前需要唤醒的父进程],
    table.cell(colspan: 2, align: center, [*线程相关*]),
    [`_clear_tid_addr`], [`CLONE_CHILD_CLEARTID` 指定的用户态地址。线程退出时清零 4 字节并 `futex_wake`，供 `pthread_join` 使用],
    [`_robust_list, _robust_list_user_addr`], [健壮 futex 链表头及其用户态地址。线程异常退出时遍历链表，对 owned mutex 标记 `FUTEX_OWNER_DIED` 并唤醒等待者],
    table.cell(colspan: 2, align: center, [*调度与 I/O*]),
    [`_priority`], [nice 值 [-20, 19]，数值越小优先级越高。同时作用于 CPU 调度和块层 I/O 排队],
    [`_sched_policy, _sched_priority`], [Linux sched ABI 策略与实时优先级；`_sched_reset_on_fork` 控制 fork 后是否恢复 SCHED_OTHER],
    [`_io_priority_override, _has_io_priority_override`], [块层 I/O 优先级覆盖值及其启用标记],
    [`_cpu_mask`], [CPU 亲和性位图，每个 bit 对应一个可用 CPU],
    [`_slot`], [当前时间片剩余量],
    [`_rlim_vec[16]`], [16 类 rlimit（CPU / FSIZE / NOFILE / NPROC 等），`prlimit64` 系统调用读写],
  ),
  caption: [PCB 字段总表]
) <tab:appendix-pcb-fields>



== 附录B　系统调用实现列表 <sec:appendix-syscall-list>


=== B.1　进程管理

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    table.cell(colspan: 2, align: center, [*进程创建与执行*]),
    [`sys_fork()`], [深拷贝地址空间，创建子进程],
    [`sys_clone()`], [按 clone flags 创建线程或进程],
    [`sys_clone3()`], [clone 扩展版本，支持更多 flags],
    [`sys_execve()`], [解析 ELF 或 shebang，装载并执行],
    [`sys_execveat()`], [execve 扩展版本，支持目录 fd 相对路径],
    [`sys_exec()`], [旧版执行接口，兼容早期二进制],
    table.cell(colspan: 2, align: center, [*进程退出与等待*]),
    [`sys_exit()`], [当前线程退出],
    [`sys_exit_group()`], [线程组全部退出],
    [`sys_wait4()`], [等待子进程状态变化],
    [`sys_waitid()`], [等待子进程，支持更细粒度选项],
    [`sys_wait()`], [旧版等待接口，兼容早期二进制],
    table.cell(colspan: 2, align: center, [*进程标识*]),
    [`sys_getpid()`], [获取进程号],
    [`sys_getppid()`], [获取父进程号],
    [`sys_gettid()`], [获取线程号],
    [`sys_personality()`], [设置进程执行域],
    table.cell(colspan: 2, align: center, [*用户与组凭据*]),
    [`sys_getuid()`], [获取实际用户 ID],
    [`sys_geteuid()`], [获取有效用户 ID],
    [`sys_getgid()`], [获取实际组 ID],
    [`sys_getegid()`], [获取有效组 ID],
    [`sys_setuid()`], [设置用户 ID],
    [`sys_setgid()`], [设置组 ID],
    [`sys_setreuid()`], [设置实际和有效用户 ID],
    [`sys_setregrid()`], [设置实际和有效组 ID],
    [`sys_setresuid()`], [设置实际、有效和保存用户 ID],
    [`sys_setresgid()`], [设置实际、有效和保存组 ID],
    [`sys_getresuid()`], [获取实际、有效和保存用户 ID],
    [`sys_getresgid()`], [获取实际、有效和保存组 ID],
    [`sys_setfsuid()`], [设置文件系统用户 ID],
    [`sys_setfsgid()`], [设置文件系统组 ID],
    [`sys_getgroups()`], [获取附加组列表],
    [`sys_setgroups()`], [设置附加组列表],
    table.cell(colspan: 2, align: center, [*会话与进程组*]),
    [`sys_getpgid()`], [获取进程组号],
    [`sys_setpgid()`], [设置进程组号],
    [`sys_setsid()`], [创建新会话],
    [`sys_getsid()`], [获取会话 ID],
    table.cell(colspan: 2, align: center, [*调度与优先级*]),
    [`sys_getpriority()`], [获取 nice 值],
    [`sys_setpriority()`], [设置 nice 值],
    [`sys_sched_yield()`], [主动让出 CPU],
    [`sys_sched_getaffinity()`], [获取 CPU 亲和性],
    [`sys_sched_setaffinity()`], [设置 CPU 亲和性],
    [`sys_sched_getparam()`], [获取调度参数],
    [`sys_sched_setparam()`], [设置调度参数],
    [`sys_sched_getscheduler()`], [获取调度策略],
    [`sys_sched_setscheduler()`], [设置调度策略],
    [`sys_getrusage()`], [获取资源使用统计],
    table.cell(colspan: 2, align: center, [*线程支持*]),
    [`sys_set_tid_address()`], [设置 clear_child_tid 地址],
    [`sys_set_robust_list()`], [设置 futex robust list 头],
    [`sys_get_robust_list()`], [获取 futex robust list 头],
    [`sys_unshare()`], [取消共享命名空间等资源],
    table.cell(colspan: 2, align: center, [*资源限制与控制*]),
    [`sys_prlimit64()`], [获取或设置资源限制],
    [`sys_prctl()`], [进程控制操作],
    [`sys_umask()`], [设置文件创建权限掩码],
  ),
  caption: [B.1 进程管理类系统调用]
)

=== B.2　内存管理

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    table.cell(colspan: 2, align: center, [*堆管理*]),
    [`sys_brk()`], [调整 heap 末端],
    table.cell(colspan: 2, align: center, [*内存映射*]),
    [`sys_mmap()`], [创建文件映射或匿名映射],
    [`sys_munmap()`], [解除映射],
    [`sys_mremap()`], [调整已有映射的大小或位置],
    [`sys_mprotect()`], [修改映射页权限],
    [`sys_madvise()`], [通知内核内存使用意图],
    [`sys_msync()`], [同步 mmap 脏页到文件],
    [`sys_mlock()`], [锁定内存页禁止换出],
    [`sys_munlock()`], [解除内存页锁定],
    [`sys_readahead()`], [预读文件数据到页缓存],
    [`sys_get_mempolicy()`], [获取 NUMA 内存策略],
    table.cell(colspan: 2, align: center, [*共享内存*]),
    [`sys_shmget()`], [创建或查找共享内存段],
    [`sys_shmat()`], [附加共享内存段到地址空间],
    [`sys_shmdt()`], [解除共享内存段附加],
    [`sys_shmctl()`], [控制共享内存段],
    table.cell(colspan: 2, align: center, [*匿名内存*]),
    [`sys_memfd_create()`], [创建匿名内存文件],
  ),
  caption: [B.2 内存管理类系统调用]
)

=== B.3　文件系统

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    table.cell(colspan: 2, align: center, [*文件打开与关闭*]),
    [`sys_openat()`], [打开或创建文件],
    [`sys_openat2()`], [openat 扩展版本],
    [`sys_close()`], [关闭文件描述符],
    [`sys_close_range()`], [批量关闭文件描述符],
    table.cell(colspan: 2, align: center, [*读写*]),
    [`sys_read()`], [读文件],
    [`sys_write()`], [写文件],
    [`sys_readv()`], [矢量读],
    [`sys_writev()`], [矢量写],
    [`sys_pread64()`], [指定偏移读],
    [`sys_pwrite64()`], [指定偏移写],
    [`sys_preadv()`], [指定偏移矢量读],
    [`sys_pwritev()`], [指定偏移矢量写],
    [`sys_lseek()`], [移动文件偏移],
    table.cell(colspan: 2, align: center, [*文件元数据*]),
    [`sys_fstat()`], [通过 fd 获取文件元数据],
    [`sys_fstatat()`], [通过目录 fd + 路径获取元数据],
    [`sys_statx()`], [扩展元数据查询],
    [`sys_truncate()`], [按路径截断文件],
    [`sys_ftruncate()`], [按 fd 截断文件],
    [`sys_utimensat()`], [设置文件时间戳],
    [`sys_fallocate()`], [预分配文件空间],
    table.cell(colspan: 2, align: center, [*目录操作*]),
    [`sys_mkdirat()`], [创建目录],
    [`sys_getdents64()`], [读取目录条目],
    [`sys_getcwd()`], [获取工作目录],
    [`sys_chdir()`], [切换工作目录],
    [`sys_fchdir()`], [通过 fd 切换工作目录],
    table.cell(colspan: 2, align: center, [*链接与重命名*]),
    [`sys_linkat()`], [创建硬链接],
    [`sys_unlinkat()`], [删除硬链接],
    [`sys_symlinkat()`], [创建符号链接],
    [`sys_readlinkat()`], [通过目录 fd 读取符号链接],
    [`sys_renameat2()`], [重命名文件或目录],
    table.cell(colspan: 2, align: center, [*挂载*]),
    [`sys_mount()`], [挂载文件系统],
    [`sys_umount2()`], [卸载文件系统],
    [`sys_statfs()`], [获取文件系统统计],
    [`sys_fstatfs()`], [通过 fd 获取文件系统统计],
    table.cell(colspan: 2, align: center, [*文件控制*]),
    [`sys_fcntl()`], [文件锁、CLOEXEC、管道容量等],
    [`sys_ioctl()`], [设备控制],
    [`sys_dup()`], [复制 fd],
    [`sys_dup3()`], [复制 fd 到指定编号],
    [`sys_flock()`], [文件锁],
    table.cell(colspan: 2, align: center, [*同步*]),
    [`sys_sync()`], [全部缓存同步到磁盘],
    [`sys_fsync()`], [单文件缓存同步],
    [`sys_fdatasync()`], [单文件数据同步],
    [`sys_sync_file_range()`], [指定范围同步到磁盘],
    table.cell(colspan: 2, align: center, [*高级 I/O*]),
    [`sys_splice()`], [两 fd 间搬运数据],
    [`sys_sendfile()`], [文件到 socket 直接发送],
    [`sys_copy_file_range()`], [文件间直接复制数据],
    table.cell(colspan: 2, align: center, [*文件权限*]),
    [`sys_fchmod()`], [修改文件权限],
    [`sys_fchmodat()`], [按路径修改文件权限],
    [`sys_fchown()`], [修改文件所有者],
    [`sys_fchownat()`], [按路径修改文件所有者],
    [`sys_faccessat()`], [检查文件访问权限],
    [`sys_faccessat2()`], [faccessat 扩展版本],
    [`sys_fchmodat2()`], [fchmodat 扩展版本],
    table.cell(colspan: 2, align: center, [*其他*]),
    [`sys_pipe2()`], [创建管道],
    [`sys_mknodat()`], [创建设备文件或 FIFO],
    [`sys_mknod()`], [旧版设备文件创建接口],
  ),
  caption: [B.3 文件系统类系统调用]
)

=== B.4　信号

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_kill()`], [向进程发送信号],
    [`sys_tkill()`], [向线程发送信号],
    [`sys_tgkill()`], [向线程组指定线程发送信号],
    [`sys_rt_sigaction()`], [注册或查询信号处理函数],
    [`sys_rt_sigprocmask()`], [阻塞或解除阻塞信号],
    [`sys_rt_sigpending()`], [查询阻塞的待处理信号],
    [`sys_rt_sigsuspend()`], [原子替换 mask 并等待信号],
    [`sys_rt_sigreturn()`], [从信号处理函数返回],
    [`sys_rt_sigtimedwait()`], [带超时的等待信号],
    [`sys_sigaltstack()`], [设置备用信号栈],
    [`sys_rt_sigqueueinfo()`], [向进程发送带数据的信号],
    [`sys_kill_signal()`], [实时信号发送接口],
    [`sys_signalfd4()`], [创建信号接收 fd],
  ),
  caption: [B.4 信号类系统调用]
)

=== B.5　时间与定时器

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    table.cell(colspan: 2, align: center, [*时间获取与设置*]),
    [`sys_clock_gettime()`], [获取指定时钟的时间],
    [`sys_clock_settime()`], [设置指定时钟的时间],
    [`sys_clock_getres()`], [获取指定时钟的分辨率],
    [`sys_gettimeofday()`], [获取墙上时间],
    [`sys_times()`], [返回进程 CPU 时间],
    table.cell(colspan: 2, align: center, [*睡眠*]),
    [`sys_nanosleep()`], [睡眠指定纳秒],
    [`sys_clock_nanosleep()`], [基于指定时钟的高精度睡眠],
    table.cell(colspan: 2, align: center, [*POSIX 定时器*]),
    [`sys_timer_create()`], [创建 POSIX 定时器],
    [`sys_timer_settime()`], [设置定时器到期时间],
    [`sys_timer_gettime()`], [获取定时器剩余时间],
    [`sys_timer_delete()`], [删除定时器],
    [`sys_setitimer()`], [设置间隔定时器],
    [`sys_getitimer()`], [获取间隔定时器状态],
    table.cell(colspan: 2, align: center, [*时间校准*]),
    [`sys_adjtimex()`], [调整内核时钟参数],
    [`sys_clockadjtime()`], [调整指定时钟参数],
    table.cell(colspan: 2, align: center, [*fd 定时器*]),
    [`sys_timerfd_create()`], [创建 fd 定时器],
  ),
  caption: [B.5 时间与定时器类系统调用]
)

=== B.6　Futex

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_futex()`], [快速用户空间互斥锁：WAIT / WAKE / REQUEUE 等子操作],
  ),
  caption: [B.6 Futex 类系统调用]
)

=== B.7　事件通知

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_epoll_create1()`], [创建 epoll 实例],
    [`sys_epoll_ctl()`], [添加 / 修改 / 删除关注 fd],
    [`sys_epoll_pwait()`], [等待关注 fd 就绪],
    [`sys_epoll_pwait2()`], [epoll_pwait 扩展版本],
    [`sys_ppoll()`], [轮询 fd 就绪状态（支持信号掩码）],
    [`sys_pselect6()`], [通知 fd 就绪状态（支持信号掩码）],
    [`sys_eventfd2()`], [创建事件通知 fd],
  ),
  caption: [B.7 事件通知类系统调用]
)

=== B.8　网络

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_socket()`], [创建 socket],
    [`sys_socketpair()`], [创建一对已连接 socket],
    [`sys_bind()`], [绑定本地地址],
    [`sys_listen()`], [标记监听状态],
    [`sys_accept()`], [取出一个连接],
    [`sys_accept4()`], [accept 带标志位],
    [`sys_connect()`], [发起连接],
    [`sys_sendto()`], [UDP 发送],
    [`sys_recvfrom()`], [UDP 接收],
    [`sys_sendmsg()`], [矢量消息发送],
    [`sys_recvmsg()`], [矢量消息接收],
    [`sys_sendmmsg()`], [批量发送消息],
    [`sys_recvmmsg()`], [批量接收消息],
    [`sys_recvmmsg_time64()`], [批量接收消息（time64 变体）],
    [`sys_getsockname()`], [获取本地地址],
    [`sys_getpeername()`], [获取对端地址],
    [`sys_setsockopt()`], [设置 socket 选项],
    [`sys_getsockopt()`], [获取 socket 选项],
    [`sys_shutdown_socket()`], [关闭读端 / 写端],
  ),
  caption: [B.8 网络类系统调用]
)

=== B.9　权限

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_capget()`], [获取线程能力集],
    [`sys_capset()`], [设置线程能力集],
  ),
  caption: [B.9 权限类系统调用]
)

=== B.10　系统信息与其他

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_uname()`], [返回内核信息],
    [`sys_sysinfo()`], [返回系统统计],
    [`sys_syslog()`], [内核日志操作],
    [`sys_getrandom()`], [获取随机数],
    [`sys_reboot()`], [重启或关机],
    [`sys_shutdown()`], [系统关机（内部接口）],
    [`sys_sethostname()`], [设置主机名],
    [`sys_setdomainname()`], [设置域名],
    [`sys_strerror()`], [获取错误码描述字符串],
    [`sys_perror()`], [打印错误信息],
    [`sys_ptrace()`], [进程跟踪与调试],
    [`sys_setns()`], [加入命名空间],
    [`sys_acct()`], [进程记账开关],
    [`sys_chroot()`], [切换根目录],
    table.cell(colspan: 2, align: center, [*调试与内部接口*]),
    [`sys_sleep()`], [内核级睡眠（调试用）],
    [`sys_uptime()`], [获取内核运行时间（调试用）],
    [`sys_userdebug1()`], [用户调试接口 1],
    [`sys_userdebug2()`], [用户调试接口 2],
    [`sys_userdebug3()`], [用户调试接口 3],
    [`sys_userdebug4()`], [用户调试接口 4],
    [`sys_userdebug5()`], [用户调试接口 5],
  ),
  caption: [B.10 系统信息与其他系统调用]
)

=== B.11　扩展属性（xattr）

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_setxattr()`], [设置扩展属性],
    [`sys_lsetxattr()`], [设置扩展属性（不跟随符号链接）],
    [`sys_fsetxattr()`], [通过 fd 设置扩展属性],
    [`sys_getxattr()`], [获取扩展属性],
    [`sys_lgetxattr()`], [获取扩展属性（不跟随符号链接）],
    [`sys_fgetxattr()`], [通过 fd 获取扩展属性],
    [`sys_listxattr()`], [列出扩展属性],
    [`sys_llistxattr()`], [列出扩展属性（不跟随符号链接）],
    [`sys_flistxattr()`], [通过 fd 列出扩展属性],
    [`sys_removexattr()`], [删除扩展属性],
    [`sys_lremovexattr()`], [删除扩展属性（不跟随符号链接）],
    [`sys_fremovexattr()`], [通过 fd 删除扩展属性],
  ),
  caption: [B.11 扩展属性类系统调用]
)

=== B.12　文件监控

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_inotify_init1()`], [创建 inotify 实例],
    [`sys_inotify_add_watch()`], [添加监控目标],
    [`sys_inotify_rm_watch()`], [删除监控目标],
    [`sys_fanotify_init()`], [创建 fanotify 实例],
    [`sys_fanotify_mark()`], [添加或删除 fanotify 监控标记],
  ),
  caption: [B.12 文件监控类系统调用]
)

=== B.13　SysV 信号量

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_semget()`], [创建或查找信号量集],
    [`sys_semctl()`], [控制信号量集],
    [`sys_semop()`], [信号量操作],
    [`sys_semtimedop()`], [带超时的信号量操作],
  ),
  caption: [B.13 SysV 信号量类系统调用]
)

=== B.14　其他高级接口

#figure(
  table(
    columns: (5cm, 9cm),
    align: (left, left),
    table.header(
      [*函数*], [*说明*]
    ),
    [`sys_bpf()`], [BPF 程序加载与操作],
    [`sys_io_uring_setup()`], [创建 io_uring 实例],
    [`sys_perf_event_open()`], [打开性能事件],
    [`sys_userfaultfd()`], [创建用户态缺页处理 fd],
    [`sys_membarrier()`], [内存屏障同步],
    [`sys_memfd_secret()`], [创建机密内存文件],
    [`sys_name_to_handle_at()`], [文件路径转句柄],
    [`sys_open_tree()`], [打开挂载树文件描述符],
    [`sys_fsopen()`], [打开文件系统配置上下文],
    [`sys_fsconfig()`], [配置文件系统上下文],
    [`sys_fspick()`], [选取已存在的文件系统配置],
    [`sys_pidfd_open()`], [通过 pid 获取 fd],
    [`sys_pidfd_send_signal()`], [通过 pidfd 发送信号],
    [`sys_keyctl()`], [内核密钥管理],
    [`sys_add_key()`], [添加内核密钥],
    [`sys_vmsplice()`], [内存到管道的矢量搬运],
    [`sys_fadvise64()`], [通知内核文件访问模式建议],
    [`sys_remap_file_pages()`], [重映射文件页（已废弃）],
  ),
  caption: [B.14 其他高级接口]
)


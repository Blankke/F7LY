
= 附录

== 重要类的具体字段解析

#figure(
  table(
    columns: (4cm, 9cm),
    align: (center, left),
    table.header(
      [*函数名*], [*含义*]
    ),
    [`get_cur_pcb`], [获取当前执行中的进程控制块],
    [`alloc_pid`], [为进程分配进程ID],
    [`alloc_proc`], [从进程池中寻找一个空进程并分配],
    [`set_killed`], [将killed字段设为1，等待杀死进程],
    [`proc_pagetable`], [为进程分配一个页表的空间并进行映射],
    [`proc_freepagetable`], [释放一个进程的页表并回收空间],
    [`either_copy_in`], [从用户空间或内核空间拷贝数据到内核空间],
    [`either_copy_out`], [从内核空间或用户空间拷贝数据到用户空间],
    table.cell(colspan: 2, align: center, [*进程操作*]),
    [`execve`], [执行新程序],
    [`growproc`], [扩展进程内存，由brk等函数调用],
    [`wait4`], [等待子进程结束],
    [`load_seg`], [加载程序段到页表映射的虚拟内存中，在执行程序时使用],
    [`sleep`], [使进程进入睡眠状态，等待某个通道或锁],
    [`wakeup`], [唤醒在指定通道或锁上睡眠的进程],
  ),
  caption: [ProcessManager接口函数说明（第一部分)]
)

#figure(
  table(
    columns: (4cm, 9cm),
    align: (center, left),
    table.header(
      [*函数名*], [*含义*]
    ),
    [`exit_proc`], [真正退出进程的逻辑，设置状态并清理资源],
    [`exit`], [当前进程或线程退出（只退出自己）],
    [`exit_group`], [当前线程组（或进程组）全部退出],
    [`clone`], [创建新进程或线程，支持克隆和线程局部存储],
    [`fork`], [创建新进程，返回新进程的PID],
    [`brk`], [扩展进程地址，设置进程的堆顶地址],
    table.cell(colspan: 2, align: center, [*文件操作*]),
    [`open`], [打开文件或设备，返回文件描述符],
    [`mkdir`], [在指定路径下创建目录],
    [`close`], [关闭文件描述符],
    [`fstat`], [获取指定文件描述符下的文件状态信息],
    [`chdir`], [改变当前工作目录],
    [`getcwd`], [获取当前工作目录路径],
    [`mmap`], [映射文件到内存],
    [`munmap`], [解除内存映射],
    [`unlink`], [删除文件或目录的链接],
    [`pipe`], [创建管道，返回读写文件描述符],
    table.cell(colspan: 2, align: center, [*其他系统调用*]),
    [`reparent`], [托孤机制，重新设置进程的父进程为init],
    [`prlimit64`], [设置或获取进程资源限制],
    [`set_robust_list`], [设置鲁棒列表，用于处理线程安全的锁],
    [`set_tid_address`], [设置线程ID地址，用于线程同步和管理],
  ),
  caption: [ProcessManager接口函数说明（第二部分)]
)

#figure(
  table(
    columns: (3.5cm, 3cm, 7cm),
    align: (center, center, left),
    table.header(
      [*字段名*], [*类型*], [*含义*]
    ),
    [`_lock`], [SpinLock], [进程控制块的锁，用于并发访问控制],
    [`_gid`], [int], [全局ID，用于在进程池中唯一标识进程],
    table.cell(colspan: 3, align: center, [*文件系统相关*]),
    [`_cwd`], [dentry\*], [当前工作目录],
    [`_cwd_name`], [string], [工作目录名称],
    [`_ofile`], [file\*], [进程打开的文件列表 (文件描述符 → 文件结构)],
    [`_fl_cloexec`], [bool], [记录每个文件描述符的 close-on-exec 标志],
    [`exe`], [string], [可执行文件的绝对路径],
    table.cell(colspan: 3, align: center, [*进程状态信息*]),
    [`_state`], [enum Procstate], [进程当前状态],
    [`_chan`], [void\*], [进程睡眠时等待的通道 (例如：某个锁或事件)],
    [`_killed`], [int], [进程是否被标记为kill (非零表示被kill)],
    [`_xstate`], [int], [进程退出状态，用于父进程wait()获取],
    [`_pid`], [int], [进程ID (Process ID)],
    [`_parent`], [Pcb\*], [父进程的PCB指针],
    [`_name`], [char], [进程名称 (用于调试)],
  ),
  caption: [PCB结构字段说明（第一部分)]
)

#figure(
  table(
    columns: (3.5cm, 3cm, 7cm),
    align: (center, center, left),
    table.header(
      [*字段名*], [*类型*], [*含义*]
    ),
    table.cell(colspan: 3, align: center, [*内存管理相关*]),
    [`_kstack`], [uint64], [内核栈的虚拟地址],
    [`_sz`], [uint64], [进程用户空间的内存大小 (字节)],
    [`_pt`], [PageTable], [用户空间的页表],
    [`_trapframe`], [TrapFrame], [保存用户态 TrapFrame 的地址 (用于系统调用和异常处理)],
    [`_context`], [Context], [保存进程的上下文信息 (寄存器等)，用于进程切换],
    table.cell(colspan: 3, align: center, [*调度相关*]),
    [`_slot`], [int], [分配给进程的时间片剩余量],
    [`_priority`], [int], [进程优先级 (0最高，19最低)],
    table.cell(colspan: 3, align: center, [*消息队列*]),
    [`_mqmask`], [uint], [用于标记进程使用的消息队列],
    table.cell(colspan: 3, align: center, [*虚拟内存区域 (VMA)*]),
    [`_vm[NVMA]`], [vma], [虚拟内存区域数组],
    table.cell(colspan: 3, align: center, [*线程/futex 相关*]),
    [`_futex_addr`], [void \*], [用于futex的唤醒],
    [`_set_child_tid`], [int\*], [线程的标志id],
    [`_clear_child_tid`], [int \*], [清除线程表示id的flag],
    [`_robust_list`], [robust_list_head \*], [用于处理线程的 robust futexes],
    table.cell(colspan: 3, align: center, [*信号处理相关*]),
    [`_sigactions[]`], [sigaction \*], [信号处理函数数组],
    [`sigmask`], [uint64], [信号掩码],
    [`_signal`], [int], [进程等待的信号],
  ),
  caption: [PCB结构字段说明（第二部分)]
)

== 系统调用实现列表

本节详细列出F7LY内核实现的210+个系统调用，按功能模块分类。

=== 进程生命周期管理

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*进程创建与执行*]),
    [`sys_fork()`], [创建新进程，完全复制父进程的地址空间、文件描述符表和进程上下文],
    [`sys_clone()`], [Linux风格的进程/线程创建接口，支持更细粒度的资源共享控制],
    [`sys_clone3()`], [clone的扩展版本，提供更多的创建选项和参数],
    [`sys_execve()`], [加载并执行新程序，替换当前进程映像，支持参数和环境变量传递],
    [`sys_exec()`], [简化版的程序执行接口],
    table.cell(colspan: 2, align: center, [*进程等待与终止*]),
    [`sys_wait4()`], [等待子进程状态改变，获取退出状态，支持阻塞和非阻塞查询],
    [`sys_wait()`], [简化版的等待接口，等待任意子进程终止],
    [`sys_waitid()`], [更灵活的等待接口，支持等待多种进程状态改变事件],
    [`sys_exit()`], [正常终止当前进程，设置退出码并释放进程资源],
    [`sys_exit_group()`], [终止整个线程组（进程组），用于多线程程序的统一退出],
    table.cell(colspan: 2, align: center, [*进程标识与信息获取*]),
    [`sys_getpid()`], [获取当前进程ID],
    [`sys_getppid()`], [获取父进程ID],
    [`sys_gettid()`], [获取当前线程ID],
    [`sys_getuid()`], [获取用户ID],
    [`sys_geteuid()`], [获取有效用户ID],
    [`sys_getgid()`], [获取组ID],
    [`sys_getegid()`], [获取有效组ID],
    [`sys_getpgid()`], [获取进程组ID],
    [`sys_setpgid()`], [设置进程组ID],
    [`sys_setsid()`], [创建新会话并设置进程组长],
  ),
  caption: [进程生命周期管理系统调用]
)

=== 内存管理系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*内存映射与解映射*]),
    [`sys_mmap()`], [内存映射核心接口，支持文件映射、匿名映射、共享内存映射],
    [`sys_munmap()`], [解除内存映射，释放指定地址范围的虚拟内存],
    [`sys_mremap()`], [重新映射内存区域，支持扩展或收缩映射大小],
    [`sys_mprotect()`], [修改内存页的保护属性，实现内存访问控制],
    [`sys_madvise()`], [向内核提供内存使用建议，优化内存管理策略],
    [`sys_msync()`], [同步内存映射文件的修改到存储设备],
    table.cell(colspan: 2, align: center, [*堆内存管理*]),
    [`sys_brk()`], [调整数据段大小，用于堆内存的扩展和收缩，是malloc库的底层实现基础],
    table.cell(colspan: 2, align: center, [*共享内存（System V IPC）*]),
    [`sys_shmget()`], [创建或获取共享内存段],
    [`sys_shmat()`], [将共享内存段连接到进程地址空间],
    [`sys_shmdt()`], [从进程地址空间分离共享内存段],
    [`sys_shmctl()`], [共享内存控制操作（获取状态、删除等）],
  ),
  caption: [内存管理系统调用]
)

=== 文件系统操作系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*文件打开与关闭*]),
    [`sys_openat()`], [相对目录打开文件，支持各种打开标志（只读、只写、读写、创建、追加等）],
    [`sys_openat2()`], [扩展版本，提供更多的打开选项和安全控制],
    [`sys_close()`], [关闭文件描述符，释放相关资源],
    [`sys_close_range()`], [批量关闭一个范围内的文件描述符],
    table.cell(colspan: 2, align: center, [*文件读写操作*]),
    [`sys_read()`], [从文件描述符读取数据到缓冲区],
    [`sys_write()`], [将缓冲区数据写入文件描述符],
    [`sys_readv()`], [矢量读取，支持多个缓冲区的批量读取],
    [`sys_writev()`], [矢量写入，支持多个缓冲区的批量写入],
    [`sys_pread64()`], [定位读取，在指定偏移位置进行读取操作，不改变文件位置指针],
    [`sys_pwrite64()`], [定位写入，在指定偏移位置进行写入操作，不改变文件位置指针],
    [`sys_preadv()`], [定位矢量读取],
    [`sys_pwritev()`], [定位矢量写入],
    [`sys_lseek()`], [设置文件位置指针],
    [`sys_sendfile()`], [在两个文件描述符间高效传输数据，零拷贝实现],
    table.cell(colspan: 2, align: center, [*文件状态与属性*]),
    [`sys_fstat()`], [获取文件描述符对应文件的状态信息],
    [`sys_fstatat()`], [获取相对路径文件的状态信息],
    [`sys_statx()`], [扩展的文件状态查询接口，提供更丰富的文件信息],
    [`sys_faccessat()`], [检查文件访问权限],
    [`sys_faccessat2()`], [扩展的文件访问权限检查],
    [`sys_readlinkat()`], [读取符号链接的目标路径],
    table.cell(colspan: 2, align: center, [*目录操作*]),
    [`sys_mkdirat()`], [创建目录],
    [`sys_getdents64()`], [读取目录项，用于ls命令的实现],
    [`sys_getcwd()`], [获取当前工作目录],
    [`sys_chdir()`], [改变当前工作目录],
    [`sys_fchdir()`], [通过文件描述符改变工作目录],
    table.cell(colspan: 2, align: center, [*文件系统管理*]),
    [`sys_mount()`], [挂载文件系统],
    [`sys_umount2()`], [卸载文件系统],
    [`sys_statfs()`], [获取文件系统统计信息],
    [`sys_fstatfs()`], [通过文件描述符获取文件系统统计信息],
    [`sys_sync()`], [文件系统同步操作],
    [`sys_fsync()`], [同步文件数据到存储设备],
    [`sys_fdatasync()`], [同步文件数据（不包括元数据）到存储设备],
  ),
  caption: [文件系统操作系统调用]
)

=== 信号处理系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*信号发送*]),
    [`sys_kill()`], [向进程发送信号],
    [`sys_tkill()`], [向特定线程发送信号],
    [`sys_tgkill()`], [向特定线程组中的线程发送信号],
    [`sys_rt_sigqueueinfo()`], [发送带数据的实时信号],
    table.cell(colspan: 2, align: center, [*信号处理设置*]),
    [`sys_rt_sigaction()`], [设置信号处理函数],
    [`sys_rt_sigprocmask()`], [设置信号屏蔽字],
    [`sys_rt_sigpending()`], [查询待处理信号],
    [`sys_rt_sigsuspend()`], [临时设置信号屏蔽字并等待信号],
    [`sys_rt_sigtimedwait()`], [等待指定信号，支持超时],
    [`sys_rt_sigreturn()`], [从信号处理函数返回],
    [`sys_sigaltstack()`], [设置备用信号栈],
  ),
  caption: [信号处理系统调用]
)

=== 网络通信系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*Socket创建与管理*]),
    [`sys_socket()`], [创建socket],
    [`sys_socketpair()`], [创建socket对，用于进程间通信],
    table.cell(colspan: 2, align: center, [*网络连接*]),
    [`sys_bind()`], [绑定socket到本地地址],
    [`sys_listen()`], [监听连接请求],
    [`sys_accept()`], [接受连接请求],
    [`sys_accept4()`], [扩展的连接接受，支持更多标志],
    [`sys_connect()`], [主动建立连接],
    table.cell(colspan: 2, align: center, [*数据传输*]),
    [`sys_sendto()`], [UDP数据发送],
    [`sys_recvfrom()`], [UDP数据接收],
    [`sys_sendmsg()`], [复杂消息发送],
    [`sys_recvmsg()`], [复杂消息接收],
    [`sys_getsockname()`], [获取socket本地地址信息],
    [`sys_getpeername()`], [获取socket对端地址信息],
    [`sys_setsockopt()`], [设置socket选项],
    [`sys_getsockopt()`], [查询socket选项],
  ),
  caption: [网络通信系统调用]
)

=== 时间管理系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*时间获取与设置*]),
    [`sys_gettimeofday()`], [获取当前时间],
    [`sys_clock_gettime()`], [高精度时间获取],
    [`sys_clock_settime()`], [高精度时间设置],
    [`sys_clock_getres()`], [获取时钟精度],
    [`sys_times()`], [获取进程时间统计],
    [`sys_uptime()`], [获取系统运行时间],
    table.cell(colspan: 2, align: center, [*定时器与睡眠*]),
    [`sys_nanosleep()`], [高精度睡眠],
    [`sys_clock_nanosleep()`], [基于时钟的高精度睡眠],
    [`sys_timer_create()`], [创建POSIX定时器],
    [`sys_timer_settime()`], [设置POSIX定时器],
    [`sys_timer_delete()`], [删除POSIX定时器],
    [`sys_setitimer()`], [设置间隔定时器],
  ),
  caption: [时间管理系统调用]
)

=== 系统信息与控制系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    [`sys_uname()`], [获取系统信息],
    [`sys_sysinfo()`], [获取系统运行状态],
    [`sys_getrusage()`], [获取资源使用统计],
    [`sys_prctl()`], [进程控制操作],
    [`sys_sched_yield()`], [主动让出CPU],
    [`sys_sched_setscheduler()`], [设置调度策略],
    [`sys_sched_getscheduler()`], [获取调度策略],
    [`sys_sched_setaffinity()`], [设置CPU亲和性],
    [`sys_sched_getaffinity()`], [获取CPU亲和性],
  ),
  caption: [系统信息与控制系统调用]
)

=== 其他重要系统调用

#figure(
  table(
    columns: (3cm, 10cm),
    align: (center, left),
    table.header(
      [*系统调用*], [*功能描述*]
    ),
    table.cell(colspan: 2, align: center, [*管道与重定向*]),
    [`sys_pipe2()`], [创建管道],
    [`sys_dup()`], [复制文件描述符],
    [`sys_dup3()`], [扩展的文件描述符复制],
    table.cell(colspan: 2, align: center, [*同步原语*]),
    [`sys_futex()`], [快速用户空间互斥锁],
  ),
  caption: [其他重要系统调用]
)

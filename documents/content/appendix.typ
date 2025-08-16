= 一些重要的类的具体字段解析

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

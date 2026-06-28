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



== 新增、改动与删除功能总表

== 2026 年系统调用变化与支持级别

== 四组合 Scoreboard 索引

== 构建、运行、Shell 与调试命令

== 图表、截图与日志来源索引

== 逐提交变更索引

// 正文待补：引用 ../commit-changelog-2026.md 与 ../report-outline-2026.md。

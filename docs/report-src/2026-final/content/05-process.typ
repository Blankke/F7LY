= 进程与线程管理

== PCB 与执行实体

=== 进程、线程组与线程标识

PCB 保存一个可调度执行实体的寄存器现场、状态、父子关系、文件表、信号状态、地址空间引用和调度信息。Linux 语义下，`_pid` 表示进程标识，`_tid` 表示线程标识，`_tgid` 表示线程组标识；同一线程组中的线程可以共享 mm、files 和 sighand，但仍有独立的 trapframe、内核栈和调度状态。

`CLONE_THREAD` 决定是否加入父线程组，`CLONE_VM` 决定是否共享 `ProcessMemoryManager`，`CLONE_FILES` 和 `CLONE_SIGHAND` 分别决定文件表和信号处理表的引用关系。将这些共享关系拆开，是 fork 进程和 pthread 线程能够使用同一 clone 入口的基础。

=== 状态与执行权

PCB 状态包括 UNUSED、USED、SLEEPING、RUNNABLE、RUNNING 和 ZOMBIE。`_running_cpu` 记录当前唯一拥有运行权的 CPU：任务被调度为 RUNNING 前必须确认该字段为空，切换出去后才清除。状态更新和执行权转移均在 PCB 锁保护下完成，避免两个 CPU 同时认领同一 PCB。

```cpp
assert(p->_running_cpu == -1);
p->_running_cpu = Cpu::current_cpu_id();
p->_state = ProcState::RUNNING;
switch_to_user(p);
// 被 yield、睡眠或退出后回到调度器
p->_running_cpu = -1;
```

这段流程表达的是“一个 PCB 同一时刻只能在一个 CPU 上执行”的不变量。它与地址空间的 `tlb_active_cpu_mask` 不同：前者描述执行权，后者描述某个 mm 当前被哪些 CPU 使用。

== SMP 调度

=== 每个 CPU 的可运行任务列表

多核调度保留每核扫描游标，并为每个 home CPU 建立可运行任务位图和压力计数。任务进入 `RUNNABLE` 状态时登记到所属 home CPU，离开该状态时清除；调度器先读取目标 CPU 的可运行任务位图，再获取 PCB 锁确认状态和执行权，避免对整个进程表进行无效扫描。

新的可运行任务根据 CPU affinity 和各 home CPU 的原子压力选择初始 CPU。该选择是 O(NUMCPU) 的，适合 8 核配置；运行期间不强制迁移任务，以保持 guest 睡眠、定时和文件操作的稳定性。目标 CPU 没有任务时，唤醒路径通过语义化 IPI 通知其重新检查可运行任务位图。

=== 调度与优先级

调度器使用轮转游标保证同一 CPU 上的 runnable 任务获得公平机会，并结合 nice/priority 计算选择顺序。默认优先级路径采用轻量原子压力读取，只有需要确认 PCB 状态时才获取锁；这样既保留优先级语义，又减少 Rust 编译期间空闲 CPU 反复扫描全局进程表的开销。

BuildStorm 冷构建诊断显示，优化前大量 CPU 时间消耗在空闲调度扫描；加入活跃 PCB 位图、初始选核和 idle/IPI 快路后，rustc 进程能够更早启动并分布到多个 guest CPU。

== 进程与线程创建

=== fork 与 clone 的共享模型

`fork()` 创建独立 mm 并通过 COW 共享物理页，文件表和信号处理表按进程语义复制；`clone()` 根据 flags 选择共享或复制。线程路径通过 `share_for_thread()` 增加同一 mm 的引用，父子进程路径通过 `clone_for_fork()` 建立新的地址空间对象。

创建过程先准备 PCB、共享对象和用户栈，再写入 parent/child tid、TLS 和 trapframe，最后一次性发布 RUNNABLE：

```cpp
Pcb *child = alloc_pcb();
child->set_memory_manager(flags & CLONE_VM
    ? parent_mm->share_for_thread()
    : parent_mm->clone_for_fork());
child->inherit_files_and_sighand(flags);
write_tid_tls_without_child_lock(child, ptid, ctid, tls);
publish_runnable(child);
```

用户内存写入可能触发缺页和睡眠，因此 `CHILD_SETTID`、`PARENT_SETTID` 等操作不能在持有未发布子 PCB 自旋锁时执行。所有失败路径只归还本次取得的 mm/files/sighand 引用，撤销子任务的 RUNNABLE 标志并释放相关锁，避免 OOM 或 fd 分配失败留下半初始化 PCB。

=== clone3 与 flags 语义

`clone3()` 复用同一共享模型，同时从结构体读取 stack、tls、set_tid、exit_signal 和 cgroup 等字段。当前实现对未实现的 namespace、pidfd 或 ptrace 语义返回明确错误，对已支持的 `CLONE_VM`、`CLONE_FILES`、`CLONE_SIGHAND`、`CLONE_SETTLS` 和 `CLONE_CHILD_CLEARTID` 保持与 clone 一致的生命周期处理，避免通过隐藏分支伪造成功。

== execve 与线程组生命周期

=== exec 替换地址空间

execve 先在新的 `ProcessMemoryManager` 中解析 ELF、PT_INTERP、动态链接器和用户栈，所有段通过 VMASpace/VmObject 建立惰性映射。装载成功后关闭 CLOEXEC fd，设置新 trapframe 的 PC/SP，再以原子方式替换当前任务的 mm。失败时保留旧 mm 和旧的 argv 状态，避免半装载映像污染进程。

线程组 leader 成功 exec 时，旧兄弟线程必须同步退出并释放它们持有的 mm、fd 和管道写端；否则父 cargo 可能永远等不到 EOF。内核使用组内退出屏障和 `_killed` 可中断等待，打断旧线程的 futex、pipe 或 nanosleep，并在所有旧线程离开共享对象后完成映像替换。

== 退出、等待与资源回收

=== clear_tid 与 futex 唤醒

设置 `CLONE_CHILD_CLEARTID` 或 `set_tid_address()` 后，线程退出时将用户态 4 字节 tid 清零，并对该地址执行 futex wake。清零和唤醒必须发生在撤销线程可运行状态之后，且不持有会阻塞用户内存访问的 PCB 锁；这样 pthread join 才能观察到稳定的退出状态。

=== wait、reparent 与僵尸进程

退出线程先进入 ZOMBIE，保留 wait 所需的退出码、tid/tgid 和资源摘要；父线程通过 wait/wait4 回收，若父线程先退出则由新的父进程接管。SIGCHLD、子进程状态变化和 wait 睡眠使用同一套父子等待锁，避免状态已经发布但父进程没有被唤醒。

=== 进程退出时回收共享内存

线程退出时先从 PCB 摘除地址空间指针，再以原子引用递减结果决定是否执行最终 `free_all_memory()`。最后一个引用归零且没有活跃 CPU 使用该地址空间后，才释放页表、VMA、VmObject 和 ASID；该规则与第四章的地址空间同步共同保证 exec、clone 失败和并发 exit 不会重复释放内存。

== 验证结果

- RV/LA 在 4G/8 vCPU 定向回归中，clone301/302/303、futex 和线程退出相关场景无内核失败；clone303 的环境差异通过标准错误码表现。
- futex WAIT/WAKE 统一使用“全局 futex 锁 -> PCB 锁”的顺序，并在锁内重新读取用户值后发布 key，消除了 BuildStorm `Compiling core` 阶段暴露的 ABBA 闭环。
- 在 RISC-V 上进行了 24 轮并发退出测试，每轮同时运行 8 个线程。测试确认多个线程同时退出时，内核能够正确回收共享地址空间和进程资源，没有出现内核崩溃、引用计数错误或重复释放；CAgent 和 Rust 工具链的进程创建、执行与退出流程也保持正常。

== 本章小结

本阶段将进程管理从单一 PCB 生命周期推进为“执行权、共享对象、地址空间和线程组状态分别管理”的模型：scheduler 负责多核执行权，clone flags 决定共享关系，exec/exit 负责线程组和 mm 的有序替换与回收，clear_tid/futex/wait 则完成用户态可观察的线程同步。

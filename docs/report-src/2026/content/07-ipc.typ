= 第七章　进程间通信

进程间通信（Inter-Process Communication，IPC）是操作系统向用户态程序提供的跨进程数据交换与事件通知机制。F7LY-OS 实现了五类 IPC 机制，按照“谁负责传递数据”“如何知道数据到达”两条线索组织：信号与 POSIX Timer 提供异步事件通知，futex 支撑用户态快速互斥与同步，管道以内核缓冲区中转字节流，共享内存绕过内核直接读写数据，epoll 与 eventfd 则统一解决“数据就绪的”通知问题。

== 信号

信号是 Unix 传统的异步通知机制，内核可向进程投递信号以告知事件发生，进程通过注册处理函数或依赖默认行为作出响应。F7LY 提供了完整的 POSIX 信号子系统，覆盖投递、排队、处理与恢复等关键语义。

=== 信号数据结构

```cpp
namespace signal
{
  
    struct signal_frame
    {
        sigset_t mask;
        TrapFrame tf;
        signal_frame *next;
    };

    struct signalstack
    {
        void *ss_sp;    
        int ss_flags;    
        size_t ss_size;  
    };

    struct kernel_sigaction_abi
    {
        union
        {
            __sighandler_t sa_handler;
            void (*sa_sigaction)(int, void *, void *);
        };
        uint64 sa_flags;
        void (*sa_restorer)(void);
        sigset_t sa_mask;
    };

```

- `signal_frame`：保存信号处理过程中的进程上下文，`mask` 记录进入 handler 前的信号掩码，`tf` 保存用户态寄存器现场，`next` 形成链表以支持嵌套信号；
- `signalstack`：描述备用信号栈的基址、大小和状态标志；
- `kernel_sigaction_abi`：`rt_sigaction` 系统调用的 ABI 传递布局，`sa_handler` 与 `sa_sigaction` 共用存储，`sa_restorer` 存放用户态恢复函数地址。

这些结构体与 POSIX 标准中的 `siginfo_t`、`sigaction`、`ucontext_t` 等语义接近，增强了对标准接口的兼容性。

=== 信号投递与处理

信号的完整处理路径分为产生、投递和恢复三个阶段。

信号由内核或用户进程通过 `kill`/`tkill` 等系统调用产生，最终调用 `add_signal()` 将目标进程的 pending 位图置位。若目标进程正在 `sigsuspend` 中阻塞等待，则被立即唤醒。

进程每次从内核态返回用户态之前，`handle_signal()` 扫描 pending 位图，找出所有未被阻塞的信号，按编号从小到大依次处理。处理方式取决于该信号的注册动作：若用户态注册了 handler，内核在用户栈上构造信号帧（保存被中断的上下文，压入信号编号和返回 trampoline 地址），然后修改 trapframe 使进程返回用户态时直接跳转到 handler；handler 执行完毕后通过 trampoline 发起 `rt_sigreturn`，内核恢复原始上下文使进程继续执行。若信号动作为默认处理（`SIG_DFL`）或忽略（`SIG_IGN`），则由内核直接执行终止、停止或忽略等操作，不进入用户态 handler。


=== 默认处理动作

当信号到达时，若进程未注册 handler 或 handler 被显式设为 `SIG_DFL`，则走默认处理路径 `default_handle()`。

```cpp
void default_handle(proc::Pcb *p, int signum)
{
    if (is_job_control_stop_signal(signum))
    {
        proc::k_pm.stop_current(signum);
        return;
    }

    SignalAction action = get_default_signal_action(signum);
    
    if (action.terminate) {
        if (action.coredump) {
            printf("[default_handle] Signal %d: Terminating process %d with core dump\n", signum, p->_pid);
        } else {
            printf("[default_handle] Signal %d: Terminating process %d\n", signum, p->_pid);
        }
        proc::k_pm.do_signal_exit(p, signum, action.coredump);
    }
}
```

`default_handle()` 的处理逻辑按信号类型分三条路径：

- *停止信号*（`SIGSTOP`、`SIGTSTP`、`SIGTTIN`、`SIGTTOU`）：由 `is_job_control_stop_signal()` 最先拦截。调用 `stop_current()` 将当前进程置为 `STOPPED` 状态，暂停执行直到收到 `SIGCONT` 唤醒；
- *终止信号*：先调用 `get_default_signal_action()` 查询该信号的默认行为。若 `terminate == true`，调用 `do_signal_exit()` 退出进程，同时根据 `coredump` 标志决定是否产生 core dump；
- *忽略信号*：若 `terminate == false`（`SIGCHLD`、`SIGURG`、`SIGWINCH` 等），函数不做任何操作直接返回，该信号在之后被 `handle_signal()` 清除。


=== POSIX Timer

F7LY 实现了两套定时器：一套是经典的 interval timer，由 `setitimer` 配置，嵌入在 PCB 中；另一套是 POSIX timer，通过 `timer_create` 创建，从全局定时器池中分配。两者的工作机制一致：用户设置到期时间和通知参数，内核在时钟中断路径上检查是否到期，到期后向目标进程发送信号。

Interval timer 嵌入在 PCB 中，每个进程有三个槽位，对应三种计时源：
- `ITIMER_REAL`（真实墙钟，到期发 `SIGALRM`）、
- `ITIMER_VIRTUAL`（用户态 CPU 时间，到期发 `SIGVTALRM`）
- `ITIMER_PROF`（用户态加内核态 CPU 时间，到期发 `SIGPROF`）。
用户通过 `setitimer` 设置初次过期值和重复间隔。每次时钟中断，`check_interval_timers()` 检查是否到期——`ITIMER_REAL` 遍历所有进程，`ITIMER_VIRTUAL` 和 `ITIMER_PROF` 只检查当前进程。到期后将对应信号置入 pending 位图，若设置了间隔则自动递推到下一轮，否则触发一次后 disarm。

POSIX timer 通过 `timer_create` 从全局数组 `g_timers[32]` 分配，比 interval timer 更灵活：时钟类型不受限制，通知方式可通过 `sigevent` 配置为发信号给进程、发给指定线程或仅更新状态，信号编号和附带 `siginfo_t`（`si_code = SI_TIMER`）也由用户指定。每次时钟中断 `check_expired_timers` 遍历全局定时器，比较 `expiry_time` 与当前时间，到期后投递信号。进程退出时自动清理其拥有的定时器。

两套定时器都在 `handle_signal()` 之前完成检查，产生的信号与其他 pending 信号一同在返回用户态时处理。

== futex

futex 是 Linux 提供的一种轻量级同步原语。它的核心思想是：将互斥锁的大部分操作放在用户态完成——线程在无竞争时仅通过一次原子比较交换就能获取或释放锁，完全不需要陷入内核。只有在线程需要等待（锁已被别人持有）或需要唤醒等待者时，才通过 `futex` 系统调用进入内核。这种设计使得 futex 在低竞争场景下几乎等同于纯用户态的原子操作，同时又能处理复杂的阻塞与唤醒逻辑。

F7LY 实现了 futex 的核心语义，包括 `FUTEX_WAIT` 与 `FUTEX_WAKE` 的基本 wait/wakeup、健壮链表（robust list）的退出自动解锁，以及 `FUTEX_REQUEUE` 的等待者迁移。

=== wait 与 wakeup

futex 的核心接口是两个函数：

```cpp
int futex_wait(uint64 uaddr, int val, tmm::timespec *timeout,
                bool timeout_is_absolute = false,
                bool use_realtime_clock = false);
int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2);
```
#figure(
  image("fig/futex.png", width: 70%),
  caption: [futex 工作机制],
) <fig:ipc-futex-mechanism>
`futex_wait` 的流程是：内核从用户态地址读出当前值，与调用者传入的期望值比较。如果两者不相等，说明在进入内核之前锁的状态已经发生了变化（可能已被其他线程释放），此时直接返回 `EAGAIN`，调用者回到用户态重新尝试获取锁。如果值匹配，表明锁仍被持有、等待是有意义的，内核将当前进程置为 `SLEEPING`，挂到与该 futex 地址对应的等待队列上。

futex 的匹配键不是裸的用户虚拟地址，而是该地址在当前页表中映射到的物理地址。这样做的原因是，多个进程可能通过 `mmap MAP_SHARED` 共享同一段物理内存，各自看到的虚拟地址完全不同，但操作的其实是同一个 futex。用物理地址作为键，`futex_wakeup` 就能正确唤醒所有共享该物理页的等待者。

`futex_wakeup` 根据用户态地址解析出 futex 键，唤醒等待队列上最多 `val` 个等待者（`val` 由调用者指定，通常为 1）。被唤醒的进程从 `futex_wait` 中醒来，返回用户态后再次尝试原子操作获取锁。

无超时的 `futex_wait` 在睡眠中会周期性被时钟中断唤醒以重检用户态值，避免因竞态导致 `futex_wakeup` 遗漏而永久挂死。带超时的版本支持相对时间和绝对时间两种模式，超时后返回 `ETIMEDOUT`。两种情况下，如果进程在等待期间收到未被阻塞的信号，均返回 `EINTR`，将控制权交还给用户态信号处理路径。

=== Robust List

当线程持有 futex 但未释放就退出时，等待该锁的线程将永久阻塞。Robust list 解决此问题：用户态 pthread 库在获取锁后将其登记到线程的 `robust_list_head` 链表中，释放时摘除；线程退出时内核调用 `futex_cleanup_robust_list()` 遍历链表，对每个属于该线程的 futex 写入 `FUTEX_OWNER_DIED` 标记，并在有等待者时唤醒它们。被唤醒的线程看到 `OWNER_DIED` 后自行调用 `pthread_mutex_consistent` 修复锁状态。

=== FUTEX_REQUEUE

条件变量的典型场景中，多个线程阻塞在 futex A 上等待信号，收到信号后需要获取互斥锁 futex B 才能继续执行。若全部唤醒，只有一个线程能成功获取 B，其余线程醒来后发现锁已被占用、立即重新阻塞在 B 上，产生大量无意义的上下文切换。

`FUTEX_REQUEUE` 将唤醒少量线程和转移剩余等待者合并为一个原子操作：从 A 的等待队列中唤醒最多 `val` 个线程，其余等待者不唤醒，直接迁移到 B 的等待队列上。如此只需一次系统调用即可完成条件变量到互斥锁的等待者交接。F7LY 在 `futex_wakeup` 中通过 `uaddr2` 和 `val2` 参数支持该语义。

== 管道

管道（Pipe）是操作系统提供的一种进程间通信（IPC）机制，允许一个进程的输出直接作为另一个进程的输入。F7LY 实现了符合 POSIX 语义的管道机制，支持匿名管道和命名管道（FIFO）。
==== 管道的基本实现
F7LY 的管道实现基于虚拟文件系统（VFS），通过 `pipe_file` 管理管道端点的读写操作。管道的核心数据结构包括：
```cpp
class pipe_file : public file
{
private:
  uint64 _off = 0;
  proc::ipc::Pipe *_pipe;
  bool _can_read = true;
  bool _can_write = false;
  bool _close_read_end = true;
  bool _close_write_end = false;
  eastl::string _fifo_path; 
  int _pipe_flags = O_RDONLY; 
public:
  pipe_file(FileAttrs attrs, Pipe *pipe_, bool is_write, const eastl::string& fifo_path = "") : 
    file(attrs), _pipe(pipe_), _can_read(!is_write), _can_write(is_write),
    _close_read_end(!is_write), _close_write_end(is_write), _fifo_path(fifo_path)
  {
    _pipe_flags = is_write ? O_WRONLY : O_RDONLY;
    lwext4_file_struct.flags = _pipe_flags;
    new (&_stat) Kstat(_pipe);
    _stat.mode = S_IFIFO | (attrs._value & 0777);
    dup();
  }
  //……
}
```

管道文件类 `pipe_file` 继承自 `file`，通过 `_pipe` 指针指向实际的管道数据结构。`O_NONBLOCK` 作为 open file description 的状态维护在 `_pipe_flags` 中，同一管道的读端和写端可各自独立设置阻塞模式。

```cpp
class Pipe
{
private:
    SpinLock _lock;
    uint8 *_buffer;             
    uint32 _pipe_size;           
    uint32 _head, _tail, _count; 
    bool _read_is_open;
    bool _write_is_open;
    Pcb *_read_waiter, *_write_waiter;   
    uint32 _read_waiter_count, _write_waiter_count;
    int pipe_flags;              
    int _async_owner_type;      
    int _async_owner_id;
    int _async_signal;           
public:
    int write(uint64 addr, int n, bool nonblock);
    int read(uint64 addr, int n, bool nonblock);
    int write_from_user(mem::PageTable &pt, uint64 addr, int n, bool nonblock);
    int read_to_user(mem::PageTable &pt, uint64 addr, int n, bool nonblock);
    int alloc(fs::pipe_file *&f0, fs::pipe_file *&f1);
    int set_pipe_size(uint32 new_size);
    void close(bool is_write);
    // ……
};
```

管道通过 `alloc()` 创建一对 `pipe_file`（读端和写端），底层共享同一个 `Pipe` 对象及其循环缓冲区。读写操作持自旋锁保护缓冲区。以写为例：缓冲区满时非阻塞模式返回 `EAGAIN`，阻塞模式记录等待者后 `sleep` 释放锁睡眠，被读端唤醒后继续。写入后唤醒读端等待者，小消息额外 `yield` 让出 CPU 以减少 ping-pong 延迟。读端关闭时写端收到 `SIGPIPE` 并返回 `EPIPE`，写端关闭时读端返回 0（EOF）。



==== 管道管理器

匿名管道只能通过 fork 在父子进程间传递文件描述符，而命名管道（FIFO）以文件系统路径为标识，任意进程只要打开同一路径即可接入同一管道。为管理 FIFO 的创建与销毁，F7LY 提供了全局单例 `k_fifo_manager`，内部维护一个 `unordered_map` 以路径为键映射到 `FifoInfo`。`FifoInfo` 记录共享的 `Pipe` 指针和当前的读者数、写者数。所有 map 操作由自旋锁保护，防止并发打开/关闭时出现竞态：

```cpp
struct FifoInfo {
    proc::ipc::Pipe *pipe;
    int reader_count;
    int writer_count;
};

class FifoManager {
public:
    proc::ipc::Pipe* get_or_create_fifo(const eastl::string& path);
    bool open_fifo(const eastl::string& path, bool is_writer);
    void close_fifo(const eastl::string& path, bool is_writer);
    bool has_readers(const eastl::string& path);
    bool has_writers(const eastl::string& path);
};
```
`get_or_create_fifo` 在路径首次打开时创建 `Pipe` 并登记到 map 中，后续打开同一路径时直接返回已有管道。`open_fifo` 和 `close_fifo` 分别递增/递减读者或写者计数，当读写两端计数均归零时管道被自动清理。`has_readers` / `has_writers` 用于打开时判断是否需要等待对端——FIFO 的语义要求读写两端都至少有一方打开才能完成打开操作。


`pipe_file` 中的 `_fifo_path` 字段用于跟踪命名管道对应的文件系统路径，使同一路径的多次打开能够准确复用同一个 FIFO 实例。`FifoManager` 提供的关键方法如下：
```cpp
    proc::ipc::Pipe* get_or_create_fifo(const eastl::string& path);
    bool open_fifo(const eastl::string& path, bool is_writer);
    void close_fifo(const eastl::string& path, bool is_writer);
    bool has_readers(const eastl::string& path);
    bool has_writers(const eastl::string& path);
    FifoInfo get_fifo_info(const eastl::string& path);
```

#figure(
  image("fig/fifomanager.png", width: 80%),
  caption: [fifomanager],
) <fig:ipc-fifo-manager>

`get_or_create_fifo` 在首次打开某路径时创建管道并登记，后续打开同一路径时直接复用已有管道。`open_fifo` 和 `close_fifo` 管理读写端计数，两端均归零后自动销毁管道。这样，命名管道既保留了“以路径标识通信端点”的 Unix 语义，又避免了重复创建底层缓冲区。

== 共享内存

共享内存允许多个进程将同一段物理内存映射到各自的地址空间中，任一进程的写入可直接被其他进程看到，无需内核中转，是数据量最大、速度最快的 IPC 方式。F7LY 同时支持 SysV 共享内存（通过 key 查找段）和 memfd（通过 fd 传递）两种使用模型。共享内存与 VMA、mmap MAP_SHARED 的协作细节已在第四章展开，本节聚焦 IPC 视角的 API 语义与内部数据结构。

=== SysV 共享内存

SysV 共享内存的思路是：内核维护一组命名的共享内存段，进程通过一个整数 key 找到目标段，将它映射到自己的地址空间，之后直接通过指针读写，数据全程走物理内存，内核不参与搬运。

```cpp
  struct shm_segment
  {
      int shmid;              
      key_t key;             
      size_t size;            
      size_t real_size;       
      u16 shmflg;            
      eastl::vector<attached_entry> attached_addrs;  
      SysvShmVmObject *object;                       
      int nattch;             
      bool auto_destroy_on_last_detach;  
      uid_t owner_uid;        
      gid_t owner_gid;
      pid_t creator_pid;      
      time_t atime, dtime, ctime;  
      // ……
  };
```

进程调用 `shmget`，传入一个 key，不同进程用同一个 key 就能找到同一个段。如果该 key 的段已经存在，直接返回它的 ID；如果不存在，需要在 flags 里带上 `IPC_CREAT` 才会现场创建一个。

每个段记录了自己的大小、访问权限（谁可读、谁可写），以及当前有哪些进程把它映射到了哪个虚拟地址。物理页不直接挂在段上，而是由一个专门的 `SysvShmVmObject` 对象管理，页面的分配和回收都走这个对象。这套 VmObject 体系也同时服务于 `mmap MAP_SHARED` 的文件映射，两种共享方式在物理页管理层面上是统一的。

`shmat` 把段挂到进程地址空间里，返回一个虚拟地址，进程直接读写这个地址就是在操作共享内存。`shmdt` 把它从地址空间里摘掉。

`shmctl` 发 `IPC_RMID` 给段打上“待删除”标记，但不会立刻销毁——如果还有进程挂着，就等最后一个 `shmdt` 离开后再清理。fork 的时候子进程自动继承父进程已经挂上的所有段。进程退出时也会自动清理自己挂着的段，不会泄漏。

=== memfd

memfd 是一种基于文件描述符的共享内存机制。发送方进程调用 `memfd_create` 创建一个匿名内存文件，拿到 fd 后通过 `mmap MAP_SHARED` 映射到地址空间，写入数据，再将 fd 经 Unix domain socket 发送给接收方——接收方以同样方式映射后，双方即共享同一段物理内存。整个过程不需要全局 key，生命周期也由 fd 的引用计数自然管理，最后一个 fd 关闭后自动回收。

*F7LY 的 memfd 实现* `sys_memfd_create` 在 `/tmp` 下利用 VFS 的 `O_TMPFILE` 创建匿名文件，并将其对外路径标记为 `memfd:<name>` 以区别于普通临时文件。参数校验遵循 Linux 约定：name 长度限制为 249 字节且不允许包含 `/`；flags 仅接受 `MFD_CLOEXEC` 和 `MFD_ALLOW_SEALING` 两个标志，其余返回 `EINVAL`。`MFD_CLOEXEC` 通过在进程的 `_fl_cloexec[fd]` 上置位实现。

每个 memfd 文件内部通过 `memfd_shared_state` 管理共享状态，包含 `seals`（32 位密封位掩码）、`sealing_allowed`（是否允许后续添加密封）和 `size`（跨 fd 共享的文件大小）。若创建时指定了 `MFD_ALLOW_SEALING`，`sealing_allowed` 置为 true；否则内核直接写入 `F_SEAL_SEAL`，表示该 memfd 已被永久封印、不允许再追加任何密封操作。若同一 memfd 通过 `/proc/self/fd` 被多次打开，多个 file 对象共享同一份 `memfd_shared_state`，通过引用计数保证密封标记和文件大小的一致性。

*密封支持* `fcntl F_ADD_SEALS` 仅对路径以 `memfd:` 开头的文件生效。若 `sealing_allowed` 为 false 则禁止添加新密封，返回 `EPERM`。`F_SEAL_WRITE` 在生效前会检查当前进程是否已将该 memfd 以 `MAP_SHARED | PROT_WRITE` 映射，若有则返回 `EBUSY`。密封一旦写入便不可撤销，后续系统调用路径上设有强制检查点：`mmap` 拒绝为带 `F_SEAL_WRITE` 的 memfd 创建写共享映射，`ftruncate`/`fallocate` 分别检查 `F_SEAL_SHRINK` 和 `F_SEAL_GROW`，`write` 在写入前检查 `F_SEAL_WRITE` 并被封印时返回 `EPERM`。这些检查点覆盖了所有修改路径，保证密封语义在全局范围内得到执行。

== 就绪通知

多路复用与事件通知是高性能 I/O 的基础设施。F7LY 实现了 epoll（Linux 的主流 I/O 多路复用接口）和 eventfd（轻量级事件计数器），二者协同工作，使进程能够高效地同时等待多个 fd 上的读写就绪事件。

=== epoll

epoll 是 Linux 的高性能 I/O 多路复用接口，解决的核心问题是：一个进程如何在大量 fd 上同时等待 I/O 事件而不消耗线性增长的 CPU 开销。`select`/`poll` 每次调用都需要将全部 fd 集合从用户态复制到内核，就绪后还需要遍历全部 fd 来找出哪些就绪，在数万个并发连接时开销不可接受。epoll 将“注册关注 fd”（`epoll_ctl`）与“等待就绪”（`epoll_pwait`）分离：fd 只在关注列表变化时才通过系统调用更新，等待时内核只扫描关注列表、无需每次从用户态传入。

F7LY 的 epoll 实例由 `epoll_file` 类表示，继承自 `file` 基类，作为 VFS 的第七种文件派生类型。其核心是一个 `eastl::vector<epoll_watch_entry>` 关注列表，每个条目记录被关注 fd、事件掩码、用户附加数据，以及 ET 模式和 `EPOLLONESHOT` 所需的状态快照。用户态与内核间的事件结构 `KernelEpollEvent` 按 64 位 Linux ABI 布局，保证批量返回事件时的内存布局一致。

epoll 不针对每种 fd 类型编写专用轮询代码，而是通过 `file` 基类上的 `read_ready()` 和 `write_ready()` 两个虚函数获取就绪状态——管道、socket、设备、eventfd 各自实现自己的就绪判断逻辑，epoll 层只做掩码匹配。这种设计使得新增 fd 类型时无需修改 epoll 核心代码。当前支持的目标类型为管道、socket、设备文件和 epoll 自身；常规文件不支持，因为它们永远就绪。

epoll 支持水平触发（LT）和边沿触发（ET）两种模式。LT 下只要 fd 处于就绪态就持续通知，编程简单；ET 下仅在就绪态从无到有时通知一次，要求用户一次性读写直到 `EAGAIN`，编程更复杂但可减少重复通知。管道的 ET 写就绪额外要求空闲空间至少达到 `PIPE_BUF`，避免逐字节消费触发大量边沿事件。`EPOLLONESHOT` 让一个条目触发一次后自动禁用，直到用户通过 `epoll_ctl MOD` 重新激活，适合多线程环境下防止同一 fd 被多个线程同时处理。epoll 还支持嵌套——一个 epoll fd 可被另一个 epoll 关注，内核通过 DFS 循环检测和最大深度限制（5 层）防止死循环和性能退化。

=== eventfd

eventfd 是一个轻量级的事件通知机制，内核中表现为一个 64 位计数器。它解决的是一个简单但常见的问题：如何让一个线程通知另一个线程“有事情要做”。信号也能做到这点，但信号是进程级别的、编号有限、传递信息量极小。eventfd 作为一个 fd 存在，可以被 `read`/`write`，可以被 epoll 监控，使用方式和普通文件描述符一致，编程模型统一。

F7LY 的 eventfd 由 `EventFdFile` 类实现，继承自 `file` 基类。其核心是一个 `SpinLock` 保护的 `uint64 _counter`。`write` 向计数器累加一个值，`read` 读出当前值并清零——这种“写累加、读清零”的对称语义使其天然适合生产者-消费者场景。`EFD_SEMAPHORE` 标志将读行为改为每次固定返回 1 并减 1，模拟信号量的 wait/post 语义。

eventfd 的关键价值在于与 epoll 的集成：`read_ready()` 在 `_counter != 0` 时返回 `true`，`write_ready()` 在计数器未达上限时返回 `true`。这意味着一个线程在 eventfd 上 `write` 后，正在 `epoll_pwait` 上等待该 fd 的另一个线程会被唤醒，从而将“计数器变化”转化为“I/O 就绪事件”。这种组合使得 eventfd 成为 epoll 的“可编程唤醒源”，广泛用于异步 I/O 框架中的任务调度和事件循环通知。

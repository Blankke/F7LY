= 02 Linux 语义实现

F7LY-OS 在本阶段把目标从 syscall 入口覆盖推进到行为语义覆盖：系统调用层负责 ABI 边界，进程、内存、文件、IPC、时间和网络模块负责真实状态变化，最后由 LTP、CAgent 和 BuildStorm 的实际路径复核。

== 验证规模

在本阶段，验证范围扩大到更完整的 Linux 通用语义：文件与挂载、进程与线程、IPC、事件通知、时间、内存映射、网络、设备 ioctl、libc 系统信息和 `/proc` / `/sys` 视图都被纳入回归。

从统计口径上看，项目从旧阶段约 297 项基础用例推进到当前约 786 项语义相关用例。这是由 scoreboard、定向 LTP 子集、CAgent 官方用例和 BuildStorm 真实执行路径共同推动的验证集合。它反映的是同一套内核对象能否在不同用户程序中保持一致行为。


== 语义增量：从 syscall 覆盖到行为语义

本阶段 Linux ABI 的共同目标，是把“入口兼容”提升为“行为兼容”，并由 LTP、CAgent 和 BuildStorm 的真实路径复核。入口兼容说明用户程序能按 Linux 编号进入内核；行为兼容还要求参数检查、错误码、对象状态、阻塞唤醒、资源回收和 `/proc` 可观察结果都能与 Linux 用户程序的预期闭合。

=== 文件 / 挂载

文件与挂载语义是本阶段最大的增量之一，覆盖约 95 项 bind / rbind / move 相关场景、48 项 fcntl 与 fd 状态场景，以及 87 项常规文件场景。挂载侧重点在 `MS_REC`、递归绑定、移动挂载和传播属性，要求路径解析、挂载树和已打开 fd 的对象引用保持一致。fd 侧重点在记录锁、`CLOEXEC`、pipe size、owner / signal 等状态，使文件描述符不只是整数句柄，而是带有生命周期和控制属性的内核对象。常规文件侧重点在 `rename`、`link`、`unlink`、`fallocate`、truncate、目录遍历和 xattr，要求元数据、页缓存和 ext4 状态同步可见。

=== 进程 / IPC / 权限

进程与 IPC 语义覆盖约 39 项进程线程场景、14 项信号场景和 22 项 SysV SHM 场景，并扩展到 robust futex、capability 和资源限制等权限边界。`clone3`、`execveat`、`waitid`、线程组、`CLONE_VM`、`CLONE_FILES`、`CLONE_SIGHAND` 和 `CLONE_CHILD_CLEARTID` 要求内核把身份、地址空间、文件表、信号处理和退出回收拆开维护。rt signal、`sigaltstack`、`tkill` / `tgkill` 和 robust futex 决定 pthread、glibc 和 Rust 运行时能否正确处理异步事件与线程退出。SysV SHM 则通过 `VmObject` 后端与 VMA 统一缺页路径连接，避免共享内存独立维护一套页生命周期。

=== 事件通知 / 时间

事件通知和时间语义覆盖约 18 项 epoll、12 项 fanotify、7 项 inotify 和 7 项 eventfd 相关场景，同时包含 signalfd、timerfd 和 `clock_*` 系列接口。epoll 不只需要 create / ctl / wait 入口，还要处理 `EPOLLET`、LT / ET 差异、`pwait` 的信号屏蔽、零超时快路和就绪事件公平返回。fanotify 与 inotify 要求文件事件 mask 能随目录项和文件状态变化交付给用户；eventfd、signalfd、timerfd 则把计数器、信号和定时器都抽象成 fd，使它们可以与 pipe、socket 和普通文件一起进入 poll / epoll。时间侧通过 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_BOOTTIME`、`clock_nanosleep` 和 POSIX timer 保持真实计时，不为评测伪造 uptime。

=== 内存 / 零拷贝

内存与零拷贝语义覆盖约 21 项 mmap / 内存场景和 7 项 splice 场景。`MAP_FIXED`、`MAP_SHARED`、`MAP_PRIVATE`、匿名映射、文件映射、`mprotect`、`munmap`、`mremap`、`msync`、`mlock` 和 memfd 都必须通过统一 VMA 模型表达。缺页路径由 `VMASpace` 查找 `VmArea`，再由 `VmObject` 准备匿名页、文件页或共享页；COW、文件尾零填充、共享页回写和大文件偏移都不能在单个 syscall 中局部处理。splice 则把 pipe 与文件之间的数据搬运纳入同一 file / page cache 语义，`SPLICE_F_MOVE`、`SPLICE_F_NONBLOCK` 和 `SPLICE_F_MORE` 只改变搬运策略和阻塞行为，不绕过 fd 生命周期与错误码规则。

=== 网络 / 设备 / libc

网络、设备和 libc 边界覆盖约 19 项 socket、7 项 ioctl 和 11 项 libc / 系统信息场景。socket 侧重点在 `accept4`、bind / connect、`sendmmsg`、socket option、socketpair、MSG flags 和阻塞 / 非阻塞语义，并通过 `/proc/net/tcp{,6}` 暴露真实连接状态。ioctl 侧重点在 TTY、`FIONBIO`、`FIONREAD` 和 `SIOCGIF*` 等设备查询与控制，使终端和网络接口信息能被 glibc 程序识别。libc / 系统信息侧覆盖 musl / glibc 适配所需的 uname、hostname、pagesize、sysinfo、getcpu、`/proc/cpuinfo`、`/proc/uptime` 和 `/sys/devices/system/cpu`，保证用户程序看到的架构、CPU、时间、内存和网络视图来自真实内核状态。

== 文件系统与挂载语义

文件系统是 Linux ABI 中最容易暴露状态不一致的部分。本阶段的工作让路径命名、挂载关系、fd 状态、扩展属性、文件数据和 ext4 元数据形成同一套可观察状态。F7LY-OS 因此在 VFS 层统一路径解析、目录项、inode、挂载点和 file 对象，再向下接入 ext4、虚拟文件、设备文件、pipe、socket 和 epoll file。

=== 挂载与路径命名

挂载语义首先影响路径解析结果。bind、rbind、move、`MS_REC` 和传播属性会改变同一路径在不同挂载树中的指向，递归挂载还要求子挂载点一起复制或移动。F7LY-OS 将挂载节点、目录项和 inode 引用分开维护：路径解析先沿挂载树找到实际文件系统，再进入对应目录项查找；已打开 fd 持有的是解析后的 file 对象引用，不会因为后续路径移动而失去原对象。

这个分层使 mount namespace 和文件对象生命周期可以分别处理。路径名变化只影响新的 lookup，已经打开的文件、目录、pipe、socket 或 epoll file 继续由 fd 表引用；最后一个引用关闭时，才进入具体 file 类型的释放路径。这样可以同时满足 shell、BusyBox、LTP 挂载用例和 BuildStorm 中大量临时目录访问的预期。

=== 常规文件操作

常规文件语义覆盖 `rename`、`link`、`unlink`、`mkdir`、`chmod`、`chown`、`truncate`、`utime`、`getdents64`、`pread`、`pwrite`、`fsync` 和 `fallocate` 等接口。它们共同修改目录项、inode 元数据、文件大小、数据块和缓存状态，不能只在 syscall 层返回一个成功值。比如 `rename` 和 `unlink` 必须使目录项缓存失效，`truncate` 和 `fallocate` 必须更新文件大小与块分配，`getdents64` 必须使用文件系统目录 cookie 维护稳定遍历位置。

数据路径上，普通 read/write 与 pread/pwrite 共用 file 对象和页缓存，但分别处理共享文件偏移与显式偏移。完整块写入、合并写回、最后 close 刷新和 fsync/fdatasync 需要保持一致：一个进程写入并关闭文件后，另一个进程通过重新 open、stat 或目录遍历看到的大小和内容应当相同。CAgent 的 fs-create、fs-readwrite、fs-directory、fs-search，以及 BuildStorm 的源码读取和中间产物写回，都依赖这个闭环。

=== fd 状态与控制语义

fd 不是单纯的整数索引，而是带状态的用户可见对象。`CLOEXEC` 决定 exec 成功后是否关闭；fcntl 记录锁决定跨进程文件互斥；pipe size 影响管道缓冲和阻塞阈值；owner / signal 与 lease 影响异步通知和文件状态变化。F7LY-OS 将这些状态保存在 file 或 fd 表关联对象中，由 fork、clone、exec、close 和 poll / epoll 路径共同维护。

不同 file 类型共享 fd 生命周期，但保留各自语义。普通文件、目录、设备文件、pipe、FIFO、socket、virtual file 和 epoll file 都能进入 fd 查找、引用计数和 close 路径；其中 pipe/FIFO 还需要根据读写端数量返回 EOF 或 EPIPE，socket 需要处理半关闭和 peer 引用，epoll file 需要释放监听关系。统一 fd 层避免了每类对象各自实现一套描述符规则。

=== xattr 与扩展元数据

扩展属性补齐了 Linux 文件系统语义中容易被忽略的一部分。get / set / remove xattr 需要在路径解析、权限检查、inode 元数据和错误码之间保持一致：属性不存在应返回对应错误，用户缓冲区过小应报告所需长度或错误，删除后再次查询不能继续命中旧缓存。

xattr 也要求缓存失效更严格。属性修改和 inode 元数据更新必须与 rename、link、truncate 等常规操作共享同一对象归属，避免目录项仍指向旧 inode 状态或后续 stat 看到过期信息。这部分体现F7LY 从“文件内容可读写”推进到“Linux 文件对象元数据可维护”。

=== statfs / fstatfs

`statfs` 与 `fstatfs` 是从固定伪造值走向真实文件系统状态的代表接口。`statfs` 根据路径解析到实际挂载，`fstatfs` 根据 fd 找到所属文件系统，再从 ext4 superblock 和挂载状态读取 blocks、free blocks、available blocks、inode 数量、fsid、文件名长度和挂载 flags。路径不存在、fd 类型错误、用户地址无效等场景分别返回 `-ENOENT`、`-EBADF`、`-EFAULT` 等 Linux errno。

这一改动直接服务 CAgent 的 fs-usage，也让 LTP 的 `statfs02/fstatfs02` 可以检查真实语义。它的意义不只是输出一组数字，而是证明 VFS 能从路径或 fd 反向找到真实挂载，并把 ext4 的可用容量、inode 与限制条件转换为 Linux ABI 结构体。

=== 完整语义链路

文件系统与挂载语义可以概括为一条链路：路径解析 -> dentry / inode -> mount namespace -> fd 状态 -> 数据路径 -> 错误码。路径解析决定访问哪个对象，dentry / inode 决定元数据和目录关系，mount namespace 决定同一路径在当前进程视图中的含义，fd 状态决定后续 exec、锁、阻塞和通知行为，数据路径决定页缓存、块缓存和 ext4 写回，错误码则决定 glibc 如何继续处理失败。

这一组语义提升使用户程序看到的不再是若干孤立 syscall，而是一套可持续变化、可查询、可回收的文件系统状态。后续进程、内存、IPC 和网络路径都建立在同一 file 对象和 VFS 语义之上。

== 进程、IPC 与权限语义

进程、IPC 与权限语义的共同目标，是让 Linux 用户程序看到稳定的执行实体、同步对象和身份环境。BuildStorm 中的 cargo、rustc、build script 和 proc-macro 会频繁创建进程与线程，glibc 和 pthread 会依赖 futex、信号、TLS、clear_tid 和 wait 语义，CAgent 与 BusyBox 则会通过 uid / gid、capability、rlimit、进程组和会话接口判断运行环境。因此，这一节不把进程看成单个 PCB，而是按“身份、地址空间、文件表、信号、IPC 对象、回收”几个维度展开。

=== 进程 / 线程

进程线程语义覆盖 `clone3`、fork、`execveat`、`waitid`、`exit_group`、进程组和会话等接口。PCB 表示一个可调度执行实体，`_pid`、`_tid` 和 `_tgid` 分别对应进程标识、线程标识和线程组标识；同一线程组中的线程可以共享地址空间、文件表和信号处理表，但仍然拥有独立 trapframe、内核栈、调度状态和退出结果。

`clone` / `clone3` 是这组语义的核心入口。`CLONE_VM` 决定是否共享 `ProcessMemoryManager`，`CLONE_FILES` 决定是否共享 fd 表，`CLONE_SIGHAND` 决定是否共享信号处理表，`CLONE_SETTLS` 决定新线程 TLS，`CLONE_CHILD_CLEARTID` 决定退出时是否清零用户态 tid 并唤醒 futex。`fork` 则创建独立地址空间，通过 COW 共享物理页；`execveat` 在新 mm 中装载 ELF、处理 `PT_INTERP` 和 shebang，成功后替换当前映像并关闭 `CLOEXEC` fd。

这一模型最重要的是失败回滚。线程创建过程中可能在分配 PCB、复制 fd 表、共享 sighand、建立 mm、写入 parent / child tid 或设置 TLS 时失败，其中用户态 tid 写回还可能触发缺页和睡眠。F7LY-OS 将子任务保持在未发布状态完成这些准备，失败时只归还本次取得的 mm / files / sighand 引用，不破坏父线程正在使用的共享对象；全部准备完成后才一次性发布为 `RUNNABLE`。这避免了半初始化线程被调度器看到，也避免了 clone OOM 或 fd 失败后留下悬空引用。

`waitid`、wait4 和 exit 路径负责把进程生命周期闭合。线程退出后进入 ZOMBIE，保留退出码、信号、tid / tgid 和父子关系状态；父进程等待时按 Linux wait status 编码回收。`exit_group` 需要杀死同一线程组中的其他线程，打断它们在 futex、pipe、nanosleep 或 epoll 上的可中断等待，避免 exec 或进程退出后仍有旧线程持有共享对象。

=== 信号

信号语义覆盖 `rt_sigaction`、`sigaltstack`、`sigpending`、`tkill` / `tgkill` 等接口。`rt_sigaction` 维护每个信号的 handler、mask 和 flags，`sigaltstack` 为用户态异常处理提供备用栈，pending 集合记录尚未交付的信号，`tkill` 和 `tgkill` 则分别按 tid 和 tgid/tid 定位目标线程。

信号交付不仅是设置一个 pending bit。内核需要在用户态返回前检查当前线程是否有未屏蔽信号，构造用户态 signal frame，保存原 trapframe、mask 和返回路径，再跳转到用户 handler；handler 返回时通过 sigreturn 恢复旧上下文。若线程正在可中断睡眠，信号还要唤醒等待路径，使 futex、nanosleep、epoll 等接口按 Linux 规则返回 `-EINTR` 或继续等待。

多线程下，信号与线程组状态必须配合。面向进程的信号要在可接收线程中选择目标，面向线程的信号必须精确投递到对应 tid；exec 和 exit 要清理或继承正确的信号状态，避免旧映像的 handler 污染新程序。CAgent 对信号压力较低，但 glibc、pthread 和 Rust 工具链会在子进程等待、线程取消、定时器和管道关闭中间接依赖这些行为。

=== SysV SHM

SysV SHM 覆盖 `shmget`、`shmat`、`shmdt`、`shmctl` 和 IPC namespace 相关语义。`ShmManager` 维护 key、权限、大小、attach 计数、删除标记和控制信息；实际物理页不由 SHM 逻辑直接安装，而是交给 `VmObject` 作为共享页后端。这样，SysV SHM 与匿名映射、文件映射共享同一 VMA / 缺页 / 页表安装路径。

`shmget` 根据 key、size、flags 和权限查找或创建共享段；`shmat` 在当前地址空间建立 VMA，并把该 VMA 连接到共享 `VmObject`；`shmdt` 解除当前进程映射并减少 attach 计数；`shmctl` 则处理状态查询、权限变更和删除标记。段被标记删除后，不能影响已经 attach 的进程继续访问；只有最后一个引用释放后，后端页面和元数据才会真正回收。

这个设计避免了共享内存另起一套页生命周期。缺页时，`VMASpace` 找到对应 `VmArea`，`VmObject` 准备共享页，页表层安装 PTE；fork、exec、exit 和 munmap 都沿同一回收规则递减引用。LTP 中的 shmget/shmat/shmctl 场景，以及 BuildStorm 中可能出现的共享映射压力，验证的是这一后端模型，而不是单个 `shmget` 入口。

=== futex 与 robust list

futex 是多线程 Linux 程序的同步接口。WAIT / WAKE 路径必须同时满足三个条件：等待前重新读取用户值，登记等待者与睡眠之间不能丢唤醒，多核下锁顺序不能形成死锁。F7LY-OS 根据私有 / 共享属性把用户地址转换为进程私有 key 或物理页 key，再放入对应 futex bucket；WAKE 只扫描匹配 key 的等待者。

本阶段重点修复了 BuildStorm `Compiling core` 阶段暴露的 futex 与 PCB 锁 ABBA 闭环。WAIT 路径统一按“futex bucket -> PCB”的顺序获取锁，在这个互锁区间内重新读取用户值、发布等待 key 并进入睡眠；WAKE 也按相同顺序筛选目标并更新线程状态。这样既消除了死锁，也避免了用户值检查和等待登记之间的丢唤醒窗口。

robust list 和 `CLONE_CHILD_CLEARTID` 负责线程退出时的同步可见性。线程退出时，内核遍历用户态 robust list，对仍由该线程持有的 robust futex 标记 owner-died 并唤醒等待者；若设置了 clear_tid，则向用户地址写回 4 字节 0，并对该地址执行 futex wake。pthread join、mutex 恢复和 glibc 线程库都依赖这些退出边界。

=== 权限与资源

权限与资源接口为用户程序提供运行环境判断。uid / gid / groups 描述当前进程身份，capget / capset 描述能力集合，rlimit 约束进程可用资源，nice 影响调度优先级，personality 影响部分 ABI 行为。

这些状态会被 glibc、BusyBox、脚本和构建工具链频繁查询。比如 Rust/Cargo 可能通过 rlimit 判断栈、文件数和内存策略，shell 工具会读取 uid/gid 决定输出或权限检查，capability 接口需要明确表达当前支持边界。对于未支持的特权操作，内核应返回 `-EPERM`、`-EINVAL` 或 `-ENOSYS` 等清晰错误，避免用户程序误以为状态已经改变。

=== 完整语义链路

进程、IPC 与权限语义可以概括为一条链路：clone / fork 建立执行实体和共享对象，exec 替换地址空间与文件表状态，信号和 futex 处理中途异步控制，SysV SHM 与 VMA 共同维护共享页面，权限和资源接口暴露运行环境，exit / wait 最终完成回收。任何一个环节只做入口兼容，都会在 glibc、pthread 或 BuildStorm 的后续路径中放大成死锁、悬空引用、错误 wait status 或用户态状态不一致。

本阶段的定向回归中，clone301/302/303、futex wait/wake、robust list、SysV SHM、线程退出和并发 mm 回收等场景均用于复核这条链路；BuildStorm 的长时间 Rust 编译则进一步验证这些语义能否在多核、高频进程创建和大量线程同步下持续工作。

== 事件通知与时间语义

事件通知与时间语义的目标，是把“外部状态变化”统一转化为 Linux 用户程序可等待、可查询、可中断的结果。文件变化、socket 收发、pipe 缓冲、信号、定时器和用户态计数器都不能各自维护一套阻塞规则，而应进入统一的 fd ready、等待队列、signal mask 和 timeout 模型。

=== epoll

epoll 是事件通知的中心接口，覆盖 `epoll_create1`、`epoll_ctl` 的 ADD / MOD / DEL、`epoll_wait` / `epoll_pwait`、LT / ET 语义和错误路径。F7LY-OS 将普通文件、pipe、FIFO、socket、eventfd、timerfd、signalfd 和 virtual file 都接入 file 对象的 ready 检查，epoll file 只保存监听关系、事件 mask 和用户 data，不复制底层对象状态。

`EPOLLET` 和 LT 的区别由“状态是否新变化 / 是否已经交付”决定。LT 事件只要对象仍可读或可写就可以重复报告；ET 事件只在状态边沿或尚未交付时报告，避免用户态在未消费数据时丢事件。`epoll_pwait` 还要在等待期间临时替换 signal mask，使“事件到达”和“信号中断”等待能够按 Linux 规则竞争返回。

零超时路径是本阶段重点优化和修复过的语义点。它不能分配用户页，不能进入可睡眠路径，也不能因为 maxevents 较大而触发内核页分配；实现上通过固定小缓冲和轮转游标收集就绪事件。轮转游标避免低编号 fd 长期占满返回数组，保证 BuildStorm 和 LTP 中大量短等待不会退化为不公平轮询。

=== fanotify 与文件事件 mask

fanotify 的核心不是文件内容读写，而是把文件系统对象上的访问、修改、打开、关闭、移动、删除等变化转成用户可观察事件。它依赖 VFS 在路径解析、dentry / inode 更新和 file close 路径中生成事件，并按用户注册的 mask 过滤。事件的对象身份、权限检查和队列容量都必须与真实文件操作同步，否则用户程序会看到“文件已经变化但通知缺失”或“事件指向旧对象”的状态分裂。

本阶段的文件语义已经把 rename、link、unlink、truncate、fallocate、close 写回和目录项缓存失效收敛到 VFS / ext4 对象生命周期中，fanotify 可以建立在这条路径上。它要求文件事件 mask 不只记录某个 syscall 名称，而要反映实际发生的对象变化：打开产生 open 类事件，写入和截断产生 modify 类事件，最后关闭产生 close 类事件，目录项移动和删除产生对应的 name 类事件。

=== inotify 与目录项变化

inotify 更关注路径和目录项层面的变化。它面向文件或目录注册 watch，通过 mask 返回创建、删除、移动、属性变化和写关闭等事件。与 fanotify 相比，inotify 更容易受到 dentry 缓存、rename 失效和目录遍历位置的影响，因此它必须和 VFS 的目录项权威更新路径绑定，而不能从 syscall 层临时拼事件。

当 `mkdir`、`rename`、`unlink`、`link`、`truncate` 或 xattr 修改发生时，VFS 需要在目录项和 inode 状态更新完成后发布对应事件；若文件被移动，watch 关系也要反映旧路径和新路径之间的语义差异。这样，用户态工具读取事件队列时，才能把 mask、name、cookie 和后续 stat 结果对应起来。

=== fd 化事件

eventfd、signalfd 和 timerfd 把不同来源的事件统一成 fd。eventfd 使用 64 位计数器表达用户态通知：写入增加计数，读取按普通模式或 semaphore 模式减少计数，计数为 0 时不可读，接近上限时不可写。它可以作为线程间唤醒对象进入 epoll，不需要额外的专用等待接口。

signalfd 将信号交付转化为文件读取。线程的 pending 信号、signal mask 和 signalfd 监听集合需要协同：被 signalfd 接管的信号应作为可读事件返回，读取时按 Linux ABI 写回 siginfo；未被接管或未屏蔽的信号仍走普通 signal delivery。这样，程序可以在 epoll 中同时等待 socket、pipe、eventfd 和信号。

timerfd 将定时器到期转化为 fd 可读事件。设置定时器时记录 clock、首次到期时间、周期和 flags；到期后增加过期计数并唤醒等待者，用户读取时得到累计 expirations。它与 POSIX timer 共享底层时间管理思路，但通过 fd 进入 poll / epoll，适合 glibc 和事件循环程序统一处理时间。

=== clock 系列与定时器

时间接口覆盖 `CLOCK_REALTIME`、`CLOCK_MONOTONIC`、`CLOCK_BOOTTIME`、`clock_gettime`、`clock_nanosleep`、clock_settime、alarm、setitimer 和 POSIX timer。`CLOCK_REALTIME` 表达墙上时间，`CLOCK_MONOTONIC` 表达单调运行时间，`CLOCK_BOOTTIME` 用于 `/proc/uptime` 和 `sysinfo.uptime` 等系统视图。F7LY-OS 不通过缩放 uptime 或伪造时间来满足评测，而是让 CAgent、BuildStorm 和 libc 读取同一套真实时间源。

POSIX timer 的创建、设置、查询、删除、到期和进程退出清理由同一 timer lock 保护。周期定时器到期时根据当前时间计算 overrun，并在信号投递或 timerfd 可读前发布；`timer_getoverrun()` 读取同一状态。没有已武装 ITIMER / POSIX timer 时，timer tick 不扫描全部 PCB 或全局定时器表，避免 8 核环境中无意义的周期性开销。

`clock_nanosleep`、futex timeout 和 epoll timeout 都依赖同一类超时等待语义：登记等待者时记录绝对或相对到期时间，timer 路径到期后唤醒目标，返回时再区分正常事件、超时、信号中断和重启条件。这样，时间语义不会被拆成每个 syscall 各自维护的局部倒计时。

=== 完整事件链路

事件通知与时间语义可以概括为一条链路：事件注册 -> 状态改变 -> fd 就绪 -> epoll wait -> 超时 / 信号屏蔽 -> 用户态返回。事件注册来自 epoll、fanotify、inotify、timerfd 或 signalfd；状态改变来自文件系统、socket、pipe、计数器、信号或时钟；fd ready 将这些变化统一成可等待对象；epoll wait 负责按 mask、LT / ET、timeout 和 signal mask 选择返回结果。

这一链路的价值在于，用户程序不需要知道底层事件来自 ext4、socket、timer 还是 signal，只需要按 Linux fd 语义等待。`epoll_wait04` 连续回归、POSIX timer、clock_settime、getitimer、futex timeout 和 CAgent 的时间 / 网络路径，复核的是这套完整行为，而不是单个等待 syscall 是否能返回。

== 内存映射与零拷贝语义

内存映射与零拷贝语义的目标，是让用户态地址空间、文件页缓存、共享内存、COW 和 pipe / 文件搬运使用同一套对象生命周期。

=== mmap 与 VMA 权限

`mmap` 语义首先由映射类型和权限决定。`MAP_FIXED` 要求在指定地址替换或覆盖旧区间，不能悄悄选择新地址；`MAP_SHARED` 要让写入对共享后端可见；`MAP_PRIVATE` 要在写访问时私有化；`MAP_ANON` 不依赖文件页源；PROT 读写执行权限则同时影响 syscall 参数校验、page fault 处理和 PTE 安装。

F7LY-OS 将这些语义收敛到 `ProcessMemoryManager`、`VMASpace`、`VmArea` 和 `VmObject`。`VmArea` 描述区间、权限、映射 flags、文件偏移和后端对象；`VMASpace` 负责 gap 查找、拆分、合并和 Maple Tree 索引；`VmObject` 提供匿名页、文件页或共享页。这样，`mmap`、`mprotect`、`munmap`、`mremap` 和 `brk` 不再各自维护一份区间状态。

=== 同步与锁页

`msync`、`mlock` / `munlock` 和 `brk` 边界属于内存映射的后续状态控制。`msync` 需要把共享文件映射中的脏页写回文件页缓存和 ext4；`mlock` / `munlock` 改变页面是否允许回收；`brk` 需要区分历史堆高水位、堆洞重增长和普通 mmap 区间，避免动态库或文件映射被错误当成 heap 扩展空间。

锁页状态要进入 VMA 或页后端，影响后续回收；同步状态要进入文件页缓存和写回路径，影响其他进程重新读取文件的结果；`mprotect` 改变权限后要同步页表和 TLB，防止旧 PTE 权限继续被远端 CPU 使用。对于 `CLONE_VM` 线程共享的地址空间，这些修改还必须和跨核 shootdown 以及 mm 引用计数配合。

=== memfd 与匿名文件

memfd 将匿名内存对象暴露为文件描述符，使它既可以被 read/write，也可以被 `mmap` 成共享或私有映射。它的大小、页后端、fd 生命周期和 seal 状态必须由同一对象维护，不能把它当成普通匿名页的临时包装。通过 `/proc/self/fd` 访问 memfd 时，用户程序看到的也应是一个稳定的 file 对象。

seal 语义进一步要求跨接口一致。`F_SEAL_WRITE` 影响 write 和可写 mmap，`F_SEAL_GROW` 影响扩展，`F_SEAL_SHRINK` 影响 truncate，后续 `fallocate`、`mmap` 和 `msync` 也要检查同一状态。这样，memfd 才能作为 glibc、构建工具和临时共享缓冲区的可靠后端，而不是只在创建时返回一个 fd。

=== splice 与 pipe / 文件搬运

splice 覆盖 pipe 与文件之间的双向数据搬运。它的语义重点是“在 fd 对象之间移动数据”，而不是绕过 VFS 或页缓存直接拷贝字节。pipe 端需要遵守环形缓冲、读写端关闭、阻塞 / 非阻塞和 poll / epoll ready 规则；文件端需要遵守文件偏移、页缓存、权限、EOF 和写回规则。

`SPLICE_F_MOVE`、`SPLICE_F_NONBLOCK` 和 `SPLICE_F_MORE` 只改变搬运策略和阻塞行为。`SPLICE_F_NONBLOCK` 不能在无数据或无空间时睡眠，`SPLICE_F_MORE` 可以提示后续还有数据，`SPLICE_F_MOVE` 允许内核优先移动页或缓冲所有权，但失败时仍要保持 Linux 可见语义。对用户程序来说，splice 的结果必须和普通 read/write 在偏移、错误码、EOF 和可读可写状态上保持一致。

=== 关键边界：COW、回写与大偏移

普通 fork 后父子共享只读物理页，写访问触发私有复制；`MAP_PRIVATE` 文件页写入不能污染原文件页缓存；`MAP_SHARED` 文件映射和 SysV SHM 则必须让多个进程看到同一后端。文件尾部不足一页的部分要按 Linux 语义零填充，越权访问应返回 SIGSEGV 或对应 errno，不能读到旧缓存内容。

决赛阶段的大内存压力还要求长度和偏移全程使用 64 位。LoongArch 内层 QEMU 会预留 8 GiB guest RAM，大文件 mmap 和 fork 后的公共缺页后备路径也会使用超过 32 位的文件偏移。F7LY-OS 将 VMA 长度、文件偏移、页偏移和后备缺页计算收敛到 64 位边界检查，保证大映射预留、首尾触页、中段 `mprotect` 和整段 `munmap` 都沿同一链路完成。

=== 完整缺页链路

内存映射与零拷贝语义最终落到一条缺页链路：page fault -> `VMASpace` -> `VmObject` -> 物理页 -> PTE 权限 -> TLB 一致性。trap 层识别用户态 page fault，地址空间通过 `VMASpace` 查找覆盖地址的 `VmArea`，检查访问权限和映射类型后调用 `VmObject::prepare_page()`，再由页表层安装 PTE。若页表或权限发生改变，架构层完成本地或远端 TLB 失效。`copy_in/copy_out`、fork COW、文件页缓存、SysV SHM、memfd 和 splice 都要尊重这条链路。它的意义在于：缺页、回写、共享、私有化和零拷贝不是互相独立的特判，而是由 VMA 描述符、Maple Tree 索引、VmObject 后端、页表权限和 TLB 同步共同决定的 Linux 内存语义。

== 网络、设备与 libc 边界

网络、设备与 libc 边界的目标，是让用户程序通过标准 Linux 接口观察到真实系统状态。

=== socket

socket 语义从 fd 开始。F7LY-OS 将 socket 纳入 VFS file 对象体系，支持 `socket`、bind / connect、listen / accept4、sendmsg / recvmsg、sendmmsg、setsockopt / getsockopt、socketpair 和 MSG flags。socket 可以和 pipe、eventfd、timerfd 一起进入 poll / epoll，也遵循 `O_NONBLOCK`、`CLOEXEC`、close、fork 继承和引用计数规则。

本机通信走 `socket_file` loopback 快路径。监听端口、连接建立、peer 引用、半关闭、接收队列和 TCP / UDP payload 都在内核中维护；accept4 返回的新 fd 必须继承正确的非阻塞和 close-on-exec 标志；sendmmsg / recvmmsg 在同一 socket 状态机上批量收发，不能绕过阻塞、错误码和对端关闭语义。

外部 IPv4 流量交给 Open-NPStack 处理 TCP / UDP / ICMP / IPv4，再通过平台 net backend 进入 VirtIO-net 或 GMAC。两条路径共享用户态 socket 接口和 fd 生命周期，底层数据面不同。`/proc/net/tcp` 与 `/proc/net/tcp6` 由受锁 socket 生命周期登记生成 LISTEN、CONNECTING、ESTABLISHED 等真实快照，不再返回固定文本；这也是 CAgent network 和网络类 LTP 用例检查的关键可观察面。

=== ioctl 与设备查询

ioctl 是设备语义暴露给 libc 和工具程序的主要边界。本阶段重点覆盖 TTY 参数、`FIONBIO`、`FIONREAD` 和 `SIOCGIF*` 等接口。TTY ioctl 决定终端输入输出模式、回显、换行转换和批量写行为；`FIONBIO` 会改变 fd 的非阻塞状态，并影响 read/write、socket recv/send 和 poll / epoll ready；`FIONREAD` 需要返回当前可读字节数，而不是固定值。

网络设备 ioctl 则负责暴露接口名、地址、flags、MTU 和 MAC 等信息。`SIOCGIF*` 查询必须和当前 net backend、loopback 设备和 `/proc/net` 视图保持一致：用户程序看到的接口存在性、地址和状态，能解释 socket bind、connect 和网络状态文件中的结果。

=== libc / 系统信息

libc 系统信息接口决定真实用户程序如何判断运行环境。

`uname` 返回系统名、release、version 和 machine 等字段。machine 必须按当前构建画像区分 `riscv64` 与 `loongarch64`，release 与 `/proc/version` 保持一致，避免用户程序通过不同入口看到相互矛盾的内核版本。CAgent 的 kernel 项和大量脚本都会通过 uname 判断当前运行平台，因此它既是格式接口，也是架构事实的出口。

`gethostname` 与 `getdomainname` 提供主机名和域名视图。它们本身不复杂，但 glibc、shell 和网络工具会把这些结果拼入提示符、日志、网络查询或环境判断。实现上需要保证长度截断、用户缓冲区写回和错误码符合 Linux 习惯，并与 `/proc` 或其他系统信息入口保持同一份命名状态。

`getpagesize` 暴露用户态页大小，是 malloc、mmap、动态链接器和 Rust 工具链判断对齐、映射粒度和缓冲策略的基础。它必须来自当前架构页表配置，而不是硬编码到某个测试值。RISC-V 与 LoongArch 的页表、TLB refill 和 ELF 段对齐都会间接依赖这个结果，错误的页大小会在动态库装载和大映射中放大成权限或偏移问题。

string / math 基础函数则支撑 libc 和用户程序的最小运行时。字符串复制、比较、查找、长度计算、内存清零 / 拷贝，以及基础整数和数学辅助函数，会出现在动态链接器、printf、路径处理、环境变量解析、Rust/Cargo 运行时和测试程序中。对 freestanding 内核和用户态基础库来说，这些函数不能依赖宿主 libc；它们需要在双架构下保持相同语义，尤其要避免越界访问、未对齐访问和与编译器内建函数约定不一致。

这些接口共同构成 libc 的环境底座。uname 告诉程序运行在哪个架构和内核版本上，hostname/domainname 提供系统命名，getpagesize 告诉程序内存粒度，string/math 基础函数保证动态链接器和普通用户程序能稳定执行。它们与 sysinfo、getcpu、`/proc/cpuinfo`、`/proc/uptime` 等接口一起，把内核真实状态转化为 glibc 和 musl 可消费的运行环境。

=== musl / glibc 适配

决赛重点是 RISC-V / LoongArch 双架构上的 glibc rootfs，历史 musl 路径仍作为兼容基础保留。musl 和 glibc 对 syscall wrapper、动态链接器路径、errno、结构体布局和 `/proc` 查询的使用习惯并不完全相同：同一个功能可能走不同 syscall 组合，也可能对错误码和返回结构体字段更敏感。

F7LY-OS 在 exec 路径处理 `PT_INTERP`，重写 musl / glibc 动态链接器路径，并支持 shebang 脚本入口；syscall 层统一使用 Linux 编号、负 errno 和 ABI 结构体布局；VFS、socket、时间、内存和进程对象则负责真实状态。这样，同一套用户程序可以在 RV / LA、musl / glibc 不同组合下复核语义。

=== 完整边界链路

网络、设备与 libc 语义可以概括为一条链路：用户程序 -> BSD Socket / ioctl / libc 查询 -> VFS file 与 syscall ABI -> 协议栈 / 设备 backend / 内核状态 -> `/proc` 与返回结构体。socket fd 负责连接和数据收发，ioctl 负责设备控制与查询，libc 信息接口负责系统视图，三者必须指向同一套内核事实。

这一层的语义提升，在于把网络和设备从“能调用”推进到“可观察、可等待、可解释”。CAgent 通过网络状态、CPU 信息、kernel version 和文件系统用量检查这些边界；BuildStorm 则通过 glibc、Cargo、proc-macro、内层 QEMU 和大量脚本持续访问它们。




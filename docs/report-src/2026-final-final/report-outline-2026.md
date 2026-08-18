# F7LY-OS 2026 决赛报告写作大纲

> 写作范围：2026-07-17 之后的决赛阶段工作，结合 `docs/report-src/2026-final/` 阶段文档、`plan_docs/final_2026_support_gap_plan.md`、2026 决赛 PPT 素材和本阶段 Git 提交记录展开。
>
> 写作目标：重写一版正式决赛报告。正文不重复初赛基础介绍，重点写决赛阶段的架构重建、Linux ABI 语义扩展、多核并发、网络链路、性能优化和验证证据。

## 00 写作主线

### 0.1 总叙事

本报告建议围绕一句话展开：

F7LY-OS 在决赛阶段从“可运行基础 Linux 程序的双架构教学内核”，推进到“能够在 RISC-V / LoongArch 双架构上承载 glibc rootfs、CAgent、BuildStorm、8 核、8 GiB 和大规模文件 / 内存 / 线程 / 网络压力的 Linux ABI 兼容内核”。

### 0.2 四条主线

1. 架构主线：从架构代码、板级资源和设备驱动混合，重构为“架构机制 - 平台画像 - 设备驱动 - 通用内核服务”的分层模型。
2. 语义主线：从 syscall 入口覆盖，推进到文件、进程、IPC、事件、内存、网络和 libc 可观察状态的 Linux 语义闭环。
3. 并发主线：从单核和低压力回归，推进到 SMP 调度、跨核 TLB、futex、等待队列、文件系统锁和页缓存一致性。
4. 性能主线：围绕 BuildStorm 暴露的瓶颈，对调度、块设备、ext4、VMA、文件页缓存、pipe 和诊断工具进行收口。

### 0.3 正文组织规则

- 每章按“原问题 - 设计改动 - 关键实现 - 验证结果”写。
- 每个技术点尽量落到具体路径、对象或提交，不只写概念。
- 证据分三类写清楚：构建通过、定向回归、完整 / 外部评测。
- 不能把诊断探针、窄测或单轮结果写成完整官方验收。

## 01 整体架构

### 1.1 项目定位与决赛目标

- F7LY-OS 是基于 xv6 思路扩展的 C++23 freestanding 教学 / 比赛内核。
- 当前支持 RISC-V 与 LoongArch 双架构。
- 面向 Linux ABI，运行 BusyBox、musl / glibc 动态程序、LTP、CAgent 和 BuildStorm。
- 决赛阶段核心约束：glibc rootfs、8 核、8 GiB、双架构、真实 Rust / Cargo 编译压力。

### 1.2 参考项目与复用边界

- 2025 武汉大学 F7LY：项目基线和延续仓库。
- LibAllocator + Buddy：页级和堆内存管理基础。
- EASTL：内核容器库，适配 freestanding 环境。
- lwext4：ext4 文件系统实现基础。
- Open-NPStack：网络协议栈基础。
- 2023 华中科技大学 AVX：sdcard 驱动参考。
- Starry-OS：I/O 调度和性能优化方向的横向参考。
- 本节要明确哪些是复用，哪些是本阶段新增边界、修复和优化，避免报告读起来像单纯移植清单。

### 1.3 当前能力总览

- 双架构启动、trap、页表、TLB、上下文切换。
- 动态 ELF、`PT_INTERP`、musl / glibc 动态链接器路径处理、shebang。
- 进程、线程、`clone` / `clone3`、futex、信号、POSIX timer。
- `mmap`、COW、SysV SHM、memfd、文件页缓存、VMA / VMObject。
- ext4 / VFS、pipe / FIFO、epoll / eventfd / timerfd、socket_file loopback、ONPS / VirtIO-net 网络链路。
- initcode 直接运行回归入口，并在结束后调用 `shutdown()`。

### 1.4 分层内核架构

- 用户态层：initcode、BusyBox、CAgent、BuildStorm、LTP。
- Linux ABI 层：syscall 编号、参数、errno、用户指针、结构体布局。
- 内核服务层：进程、内存、文件、IPC、时间、网络。
- 平台画像层：`mk/platform/` 与 `kernel/platform/<arch>/<board>/`。
- 架构机制层：boot、trap、页表 / TLB、上下文切换。
- 设备驱动层：VirtIO、GMAC、AHCI、PLIC、ExtIOI、LIOINTC、UART。

### 1.5 架构重建：平台画像与编译解耦

- 原问题：`ARCH`、`BOARD`、initcode、链接脚本、驱动选择在旧 Makefile 中耦合。
- 新模型：`make <动作> PROFILE=<完整平台画像> MODE=<启动模式>`。
- 画像包括 CPU 架构、板级目录、设备驱动、链接脚本、启动入口。
- 构建系统用 `build-config.stamp` 和 `kernel-sources.list` 记录配置和源码集合。
- 关键提交可引用：`6fc57706`、`35389e56`、`69dc9305`、`43a671bd`。
- 配图建议：构建画像架构图。

### 1.6 DTB 驱动的运行时硬件发现

同一个硬件事实只保留一个权威来源：CPU/内存来自 DTB，板上固定资源来自平台画像，运行状态归设备驱动。

① 固件输入
启动入口只接收固件传入的 DTB 物理地址，并转换为统一 BootInfo。
② 统一解析
DtbManager 解析 memory / reserved-memory、CPU hart、timebase、PLIC context、initrd 与网卡 MAC 等事实。
③ 子系统消费
PMM 排除 DTB 与保留区后建立可分配 RAM；SMP 只启动 DTB 声明的 CPU，设备层不再重复硬编码拓扑。

### 1.7 VMA 记录重构：索引与后端解耦

- 原问题：mmap、mprotect、munmap、page fault、fork COW 和文件映射路径容易维护多份状态。
- `VmArea`：描述区间、权限、映射类型、页偏移和增长策略。
- `VMASpace`：统一创建、拆分、合并、gap 查找和 Maple Tree 索引。
- `VmObject`：Anon / File / SysV SHM 三类 VmObject 各自实现 prepare_page，将匿名页、文件页缓存和共享内存的缺页语义放到对应后端。

- 关键提交可引用：`f4aa440b`、`d092dec1`、`1966b4ac`。

## 02 工作一：Linux 语义实现

### 2.1 验证规模：297 到 786

- 2025 阶段主要围绕 basic 对应 syscall 内容做 LTP 验证。
- 2026 阶段扩展到更多 Linux 通用语义，验证从入口覆盖转向行为覆盖。
- 可以用“297 -> 786”作为总览数字，但正文要说明统计口径和来源。

### 2.2 从 syscall 覆盖到行为语义

共同目标：提升Linux ABI 的“入口兼容”,并由 LTP 复核，兼容各类linux用户软件程序。

- 本章建议按不同语义展开：文件 / 挂载、进程 / IPC / 权限、事件通知 / 时间、内存 / 零拷贝、网络 / 设备/ libc 

### 2.3 文件系统与挂载语义

- mount / bind / rbind / move / `MS_REC` / namespace。
- 常规文件：rename、link、mkdir、chmod、chown、truncate、utime、getdents。
- fd 状态：`CLOEXEC`、记录锁、pipe size、owner / signal、lease。
- xattr：get / set / remove。
- `statfs` / `fstatfs` 从真实 ext4 挂载读取统计信息。
- 路径解析 -> dentry / inode -> mount namespace -> fd 状态 -> 数据路径 -> 错误码。

语义提升：覆盖路径命名、挂载传播、fd 状态、扩展属性和文件数据路径，补齐 Linux 文件系统最复杂的一组行为。
### 2.4 进程、IPC 与权限语义

- 进程 / 线程：`clone3`、fork、execveat、waitid、exit_group、进程组 / 会话。
- 信号：`rt_sigaction`、`sigaltstack`、`sigpending`、`tkill` / `tgkill`。
- SysV SHM：`shmget`、`shmat`、`shmctl`、IPC namespace。
- futex：WAIT / WAKE、robust list、超时、信号中断。
- 权限与资源：uid / gid / groups、capget / capset、rlimit、nice、personality。
- 写作重点：进程语义被拆成身份、地址空间、文件表、信号、IPC 对象和回收五个维度。

### 2.5 事件通知与时间语义

- epoll：create1、ctl ADD / MOD / DEL、wait / pwait、LT / ET、公平轮转。
- 文件通知：fanotify、inotify、事件 mask。
- fd 化事件：eventfd、signalfd、timerfd、NONBLOCK、CLOEXEC、SEMAPHORE。
- 时间接口：CLOCK_REALTIME、CLOCK_MONOTONIC、CLOCK_BOOTTIME、clock_nanosleep、alarm、setitimer。
- 写作链路：事件注册 -> 状态改变 -> fd 就绪 -> epoll wait -> 超时 / 信号屏蔽。

语义提升：核心不是增加等待接口，而是把外部变化、信号、定时器统一纳入 fd 驱动的可阻塞事件模型。

### 2.6 内存映射与零拷贝语义

- mmap：MAP_SHARED / PRIVATE / ANON / FIXED、PROT、mmapstress。
- 同步与锁页：msync、mlock / munlock、brk 边界、memfd。
- splice：pipe 与文件双向搬运，`SPLICE_F_MOVE`、`NONBLOCK`、`MORE`。
- 关键边界：权限错误、文件尾零填充、共享 / 私有页回写、COW。
- 写作链路：page fault -> VMASpace -> VmObject -> 物理页 -> PTE 权限 -> TLB 一致性。
语义提升：VMA 描述符、Maple Tree 索引和 VmObject 后端共同决定缺页、回写、共享与 COW 语义。

### 2.7 网络、设备与 libc 边界

- socket：accept4、bind / connect、sendmmsg、setsockopt、socketpair、MSG flags。
- ioctl：TTY 参数、FIONBIO、FIONREAD、SIOCGIF*。
- libc / 系统信息：uname、gethostname、getdomainname、getpagesize、sysinfo、getcpu。
- `/proc` 与 `/sys`：CPU、uptime、version、net/tcp、net/tcp6。
- 双 libc 验证
RV/LA × musl/glibc
同一测例验证深度翻倍

用户程序-> BSD Socket -> VFS/File -> 协议栈 -> 设备/libc

语义提升：socket fd、设备 ioctl 和 libc 查询共同服务真实用户态程序，形成网络与设备兼容闭环。
## 03 工作二：网络链路

### 3.1 网络模块分层
网络系统以 Linux 兼容 Socket 为统一入口，向下分成本机 loopback 快路径和 ONPS / VirtIO-net 外部 IPv4 路径两条数据面。本系统支持 AF_INET、AF_UNIX 等多种地址族的套接字操作，TCP / UDP / ICMP 与设备收发分别由 socket_file、协议栈和网卡后端协同完成。

整个网络系统主要包含以下几个核心组件：
1. VFS 集成层
2. BSD Socket 接口
3. socket_file 分流层
4. ONPS 协议栈与网络适配层
5. VirtIO-Net / GMAC 设备驱动层

网络系统采用分层架构设计，从底层硬件驱动到上层应用接口，形成了完整的网络协议栈。


### 3.2 标准 BSD Socket 接口
F7LY-OS实现了完整的POSIX标准BSD Socket接口，为用户程序提供标准化的网络编程API。通过VFS文件系统抽象，Socket被视为特殊文件，支持统一的文件操作接口，实现了"一切皆文件"的设计理念。

### 3.3 网络协议栈与VirtIO-net

网络模块以 Linux 兼容 Socket 为统一入口，向下分为两条数据路径：

1. 本机通信走 socket_file loopback 快路径，直接通过端口表、peer 连接和缓冲队列传递 TCP/UDP payload；

2. 外部 IPv4 流量交由 ONPS 协议栈处理TCP/UDP/ICMP/IP，再通过 VirtIO-Net 收发完整以太网帧。

该设计兼顾可验证的本机数据面与后续外网扩展能力。

## 04 工作三：多核代码

### 4.1 CPU 虚拟化与拓扑

DTB 识别 CPU/hart
统一 QEMU 与真板拓扑来源
possible CPU 不再写死

online / possible mask
调度与亲和性只面向 online CPU
同时驱动 /proc 与 /sys 信息生成

per-CPU PLIC 与本地状态
claim/complete 基于当前 CPU context 
current、irq_depth、timeslice 各核独立

per-CPU 内存分配
把高频页 / 对象分配尽量留在本核
降低共享分配器热点

### 4.2 SMP 调度

- runnable 集合、home CPU、初始选核和跨核唤醒。
- `kick_cpu` / IPI 用于让远端 CPU 及时调度。
- 避免运行期激进迁移破坏 guest sleep / 时间稳定性。
- BuildStorm 中 cargo / rustc / proc-macro 进程暴露调度热点。

### 4.3 内存模型与屏障

- 页表更新先写 PTE，再做本地或远端 TLB shootdown。
- `eastl::atomic` 和 acquire / release 用于发布 mask、refcnt、flags。
- 缺页 / COW / mmap 并发 MM / VMA / VmObject 依靠锁与引用计数 稳定处理 page fault 与地址空间修改

- 跨核延迟回收
 页表、ASID、MM、trapframe、kstack
 必须等 ack 或引用归零后回收

### 4.4 等待队列与跨核唤醒

跨核唤醒
目标线程不在当前核时调用 kick_cpu
通过 IPI 让远端 CPU 及时调度

超时等待优化
由统一时间路径检查到期等待者
不再每个 tick 广播唤醒所有线程

分桶等待队列
futex key / 等待对象进入对应 bucket
唤醒时只扫描相关等待者

sleep / wakeup 顺序
入睡、解锁、切换顺序固定
唤醒时同时更新线程状态与等待队列

### 4.5 锁拆分与死锁规避

- 不用单一大锁覆盖全部内核状态:重点是对象锁、分桶和 per-CPU 状态
把热点路径从单一全局锁上拆开
- 文件系统：ext4 保留元数据锁，同时拆出 file lock cache lock、fd/object lock 与 pipe lock
- 进程 / futex / 内存：PCB lock、wait lock、futex bucket、mm lock、VMA lock、object lock 分离。
- bucket 按编号加锁；pipe 锁内避免缺页；shootdown 完成后再释放跨核对象。



## 05 工作四：性能优化



### 5.1 块设备驱动与 I/O 调度

统一块设备接口
BlockDevice::read/write_blocks
平台差异收敛到 block backend

128 深度 VirtQueue
descriptor chain 多请求 in-flight
有空闲描述符即持续 dispatch

mClock 软件调度
8 service class · R/W/L 标签
class 内 per-flow RR，避免单进程独占

完成与唤醒
used ring → completed → wakeup
回收 descriptor 后继续派发

连续块提交
lwext4 4KiB blocks → 128KiB bounce
合并为一次 sector transfer

### 5.2 Ext4 与文件页缓存

缓存 clean page
全局 cache 页保持只读；MAP_PRIVATE 写入先私有化，避免共享 cache 页被原地改脏。

回收与预读
每个 shard 独立 LRU；内存紧张可主动 reclaim clean pages；顺序 miss 触发 16 页预读。

锁粒度
读锁 + bcache 独立锁

复用路径
read / mmap / exec 复用

### 5.3 Pipe：连续缓冲与等待唤醒

1. 数据搬运：从逐字节到连续 chunk
ring buffer 只在 head/tail 回绕处分段
一次 memmove 搬运“当前连续可读/可写区间”
直接 user buffer ↔ pipe buffer，减少中转与取模

2. 容量策略
Linux 兼容默认 4KiB；支持 F_SETPIPE_SZ 动态调整，最大 64KiB。性能优化不靠破坏默认语义。

3. poll / epoll 状态
可读 / 可写阈值直接由 count 与容量判断；写入、读出、close 后同步更新等待与异步通知。

### 5.4 VMA：索引、合并与文件映射

- syscall -> VMASpace -> Maple Tree -> VmObject。
- 相邻匿名 VMA 合并，gap 搜索由索引承担。
- 文件映射优先进入 clean page cache，写访问再 COW / private overlay。
- 8 GiB 大映射、mprotect、munmap、mremap、文件偏移使用 64 位长度和偏移。
- 缺页、PTE、Cache、TLB 一致性要作为完整链路说明。


## 06 亮点与总结

### 6.1 当前成果概述

- 当前报告正文已经覆盖 01 到 05 章，分别对应整体架构、Linux 语义、网络链路、多核并发和性能优化。
- 现阶段核心成果是把 CPU 拓扑、SMP 调度、页表 / TLB、等待唤醒、块设备、ext4、pipe、VMA 和网络链路收束到一套 Linux 兼容路径。
- 关键验证对象已经从单点 syscall 扩展到 glibc rootfs、CAgent、BuildStorm、shell、gcc、rustc、vim 和 git 等真实用户态程序。

### 6.2 已验证能力

- 双架构启动、动态 ELF、`PT_INTERP`、shebang 和 Linux ABI syscall 路径已经形成闭环。
- `socket_file` loopback 快路径与 ONPS / VirtIO-net 外部 IPv4 路径已经分流明确。
- ext4、文件页缓存、pipe、VMA / VmObject、futex、epoll 和跨核调度已经可以共同承载长链路压力。

### 6.3 后续工作重点

- 继续收敛 Linux ABI 中仍需细化的边界行为和长尾语义。
- 继续提升高并发、多核和大内存场景下的稳定性与可预测性。
- 继续扩展真实用户程序和外部网络场景的端到端覆盖面。

= F7LY-OS 内核概述

== 项目目标与 2026 年演进方向

F7LY-OS 是 C++ 编写的支持 RISC-V 与 LoongArch 双架构的宏内核模块化操作系统，面向教学、比赛、Linux ABI 兼容与连续测例评测。项目从零参考 xv6 构建，持续演进至可运行动态链接的用户程序、BusyBox 交互式 Shell 及大规模 LTP 连续测例。

2026 年的主线目标是在双架构一致性、Linux ABI 正确性、长连续测例稳定性和 I/O 与线程性能四个方向上取得实质进展。相比于 2025 年的"可启动、可运行基础测例"，2026 年的内核已具备：

- 完整的动态 ELF 装载链，支持 musl / glibc 双 C 运行库和 `#!` 脚本解释器递归；
- 真实可用的网络协议栈，通过 iperf / netperf 吞吐验证, 可正常访问web服务；
- 双架构统一 virtio-blk 框架与优先级带宽借用 I/O 调度器；
- 交互式 BusyBox ash 终端，支持控制台输入、文件浏览和脚本执行；
- 四组合（RV+musl、RV+glibc、LA+musl、LA+glibc）scoreboard 评测体系，覆盖 LTP、libcbench、iozone、lmbench、cyclictest、iperf、netperf、unixbench、lua、BusyBox、libctest 连续测例。

内核当前为 C++23 freestanding 环境，显式禁用异常和 RTTI（`-fno-exceptions`、`-fno-rtti`），以 GCC 工具链编译。系统调用严格遵循 Linux raw syscall 规范，累计实现并验证超过 240 个系统调用。

// TODO: 从 draw.io 导出 f7ly-2026-architecture.drawio → f7ly-2026-architecture.png
// 当前 PNG 未生成，架构图暂以 drawio 源文件描述，见 fig/ 目录
//
#figure(
  image("fig/f7ly-2026-architecture.png", width: 100%),
  caption: [F7LY-OS 2026 年整体架构图],
) <fig:overview-architecture>


== 当前系统能力总览

*双架构支持*——RISC-V 与 LoongArch 均以 QEMU virt 平台为主要验证目标，同时在 VisionFive 2（RISC-V）和 LS3A5000 / LS2k1000（LoongArch）实机硬件上进行了启动验证。两架构共享同一套内核核心逻辑，差异封装在 HAL（`kernel/hal/`）和架构特定目录中。

*内存管理*——伙伴系统（Buddy）管理物理页，内核堆基于 liballoc 提供任意大小分配，VMA 子系统以描述符→Maple Tree 索引→后端对象（`VmObject`）三层架构管理用户地址空间。支持 mmap、munmap、mprotect、COW、惰性分配、共享内存、`brk` 堆和栈自动扩展。

*进程与线程*——统一 `Pcb` 抽象表示进程和线程。支持 `fork`、`clone`/`clone3`、`execve`、`wait4`/`waitid` 完整生命周期。线程通过 `CLONE_VM | CLONE_THREAD` 创建，共享地址空间，支持 futex、健壮 futex、`CLONE_CHILD_CLEARTID` 等 pthread 所需语义。

*IPC*——信号（含实时信号、`sigqueue`、`siginfo_t`）、POSIX timer、System V 共享内存（`shmget`/`shmat`/`shmdt`/`shmctl`）、匿名管道、有名管道（FIFO）、eventfd、memfd、epoll。

*文件系统*——VFS 统一文件操作接口，支持 ext4（根文件系统，基于 lwext4）和 FAT32（数据盘）。`File` 抽象类通过 C++ 多态支持普通文件、管道、socket、虚拟文件（`/proc`）和 epoll 文件。支持 `ioctl`、`fcntl`、`flock` 文件锁、`splice`/`sendfile` 零拷贝搬运、`fanotify` 通知、loop 设备、ramdisk 等。

*网络*——支持本机loopback及网络web访问。loopback 路径由 `socket_file` 直接实现，不经过 VirtIO 网卡和 Ethernet/IP 层：TCP 通过端口表查找 listener、创建 server-side socket、互设 peer，并以 `_recv_buffer` 完成双向字节流传输；UDP 以 `loopback_datagram` 队列保持消息边界和源地址回填；AF_UNIX 复用本地可靠 stream 队列。非 loopback IPv4 流量在网络栈初始化成功后交给 ONPS，再经 `virtio0` 适配层和 VirtIO-Net 驱动收发完整以太网帧。BSD Socket ABI 支持 socket/socketpair、bind/listen/connect/accept、send/recv/sendmsg、sendmmsg/recvmmsg、常用 socket option、socket ioctl 兼容视图，以及 poll/epoll 就绪通知。iperf 和 netperf 在双架构均可运行。

*用户态*——支持 musl 和 glibc 两种 C 运行库的动态链接程序。用户态入口包括自动连续测例 initcode 和交互式 BusyBox ash，后者使用独立 ext4 rootfs 镜像，支持命令行浏览、脚本执行和正常退出。支持通过 `apk` 包管理器安装 Alpine Linux 软件包。

*工程体系*——四组合 scoreboard 追踪 LTP 评测进度，日志保存到 `logs/run/`，提供 LTP runner/parser/ranker 工具链实现可重复的批量分析流程。

== 2026 年整体分层架构

F7LY-OS 的新架构图按数据流和依赖关系分为六层，自上而下为：

=== 用户态层

最顶层，运行在 U-Mode。包括 BusyBox ash 交互式 Shell、自动连续测例（LTP、libcbench、iozone、lmbench、cyclictest、iperf、netperf、unixbench、lua、BusyBox、libctest）和通用用户程序。三者共享同一套 Linux ABI，但 Shell 模式使用独立 rootfs 镜像以避免污染评测环境。

=== 系统调用层 / Linux ABI

系统调用分三组模块：`sysio`（I/O 类，如 read/write/open/close/socket）、`sysproc`（进程类，如 fork/clone/exec/wait/signal/timer）、`sysfile`（文件系统类，如 mount/stat/link/xattr）。每组模块负责参数校验、权限检查和调用核心服务。底层通过统一的 `syscall_handler` 表分发，两架构的各系统调用号保持一致。

=== 内核服务层

内核核心功能的集中承载层：

- *地址空间（AddrSpace）*：封装进程页表、VMA 空间和 VmObject 后端对象，是内存管理的统一入口。
- *进程内存管理器（Proc Mem manager）*：每个进程的 `ProcessMemoryManager` 实例，管理 ELF 段装载、堆、栈、mmap 区和共享内存附加。
- *共享内存（SHM）*：System V 共享内存的全局管理，支持 key→段 ID 查找和跨进程附加。
- *调度器（Scheduler）*：轮转 + 优先级调度，nice 值同时影响 CPU 和 I/O 调度。
- *IPC*：futex、信号、管道等进程间通信原语。
- *VFS*：虚拟文件系统层，统一转发到 ext4、FAT32、SysFS 等具体文件系统。pipe、file、socket 三种 `File` 子类在此层注册并对外暴露统一接口。

=== 内核基础设施层

提供内核自身运行所需的服务和抽象：

- *物理内存管理（PMM）*：Buddy 伙伴分配器，以 4KB 页为单位管理物理内存。
- *虚拟内存管理（VMM）*：封装页表操作和 TLB 管理。
- *VMA 空间*：以 Maple Tree 索引加速 VMA 查找和区间搜索。
- *锁机制（Locks）*：SpinLock 和 SleepLock，保护内核临界区。
- *时钟（Ticks）*：timer tick 驱动调度和定时器。
- *块设备支持*：Ramdisk、LoopDev，以及通过 VirtIO-Blk 访问的磁盘。
- *标准 I/O*：stdin/stdout/stderr 经控制台/UART 与用户态交互。
- *网络基础设施*：ONPS 网络协议栈 + VirtIO-Net 驱动。

=== 基础层

直接与硬件交互的底层模块：

- *HAL（硬件抽象层）*：封装 CPU 操作、上下文切换、地址转换和架构特定寄存器访问。
- *MMU*：页表硬件操作的直接封装，RISC-V 走 Sv39，LoongArch 结合 DMWIN 直映窗口。
- *Trap Manager*：统一中断/异常/系统调用入口，分发到对应处理函数。
- *Console / UART*：控制台行规程和 SBI/UART 输入输出。
- *Dev Driver*：设备驱动框架，管理字符设备、块设备和流设备的注册和访问。

=== 硬件平台层

QEMU virt 机器是主要验证平台（RISC-V 和 LoongArch），同时内核已在 VisionFive 2（RISC-V 实机）、LS3A5000 和 LS2k1000（LoongArch 实机）上成功启动并进入用户态。

== 参考的代码

F7LY-OS 在开发过程中参考和移植了以下开源项目与第三方库：

- *xv6-2021*：MIT S081 课程的教学内核 #link("https://github.com/mit-pdos/xv6-public")[xv6-2021]。异常处理机制、内核地址布局和伙伴分配器算法均参考其设计，并在双架构和 C++ 环境下重新实现。

- *lwext4*：C 语言编写的 ext4 文件系统库 #link("https://github.com/gkostka/lwext4")[lwext4]，经 C++ 适配与重构后作为 F7LY 的主要文件系统后端。

- *Open-NPStack (ONPS)*：嵌入式网络协议栈 #link("https://gitee.com/Neo-T/open-npstack")[ONPS]，提供 TCP/IP/UDP/ICMP 等协议的处理框架，结合 VirtIO-Net 驱动形成 F7LY 的网络子系统。

- *EASTL*：Electronic Arts 标准模板库 #link("https://github.com/electronicarts/EASTL")[EASTL]，在内核不支持标准 C++ 库的环境下提供 vector、list、map、hash_map 等高效数据结构。

- *liballoc*：轻量级堆分配器，经集成后作为内核堆内存管理的细粒度分配后端。

- *XN6*：2024 年武汉大学团队的 LoongArch xv6 移植项目 #link("https://gitlab.eduxiji.net/T202410486992576/OSKernel2024-2k1000la-xv6")[XN6]，在 LoongArch 页表管理和硬件抽象层设计上提供了重要参考。

- *Starry-OS*：2024 年 OSCOMP 冠军内核 #link("https://github.com/Starry-OS/StarryOS.git")[StarryOS]，在 I/O 调度和性能优化思路上提供了横向对比参考。

此外，在性能优化（iozone、libcbench）阶段参考了 OSCOMP 2026 排行榜上其他参赛内核的优化思路，以及官方 autotest-for-oskernel #link("https://github.com/oscomp/autotest-for-oskernel")[评测仓库]中的 baseline 评测机制。

== 我们的工作

2026 年度工作由三个队员共同开发完成，在 2025 年基线之上进行了大量重构和新增功能。截至 2026 年 6 月，第二阶段集中开发共 96 个提交，改动 388 个文件（内核 146 个文件），系统调用绑定由 224 个增至 243 个。主要工作包括：

- *VMA 与进程地址空间重构*——引入 `VmArea` 描述符、`VmaMapleTree` B+Tree 索引和 `VmObject` 后端对象三层架构，统一管理 ELF 段、堆、栈、mmap 区和共享内存附加。将缺页处理、COW、惰性分配的路径收敛到统一的 `fault_page` 入口。

- *双架构统一 virtio-blk 框架*——将 RISC-V MMIO 和 LoongArch PCI 的 virtio-blk 传输差异封装为 transport 适配层，通用层统一管理 request、descriptor、completion 和 buffer 回写。在此基础上建立了多优先级、按进程 flow 轮转的 priority-borrow I/O 调度器，nice 值同时影响 CPU 和磁盘带宽分配。

- *网络数据面与 Socket ABI 完善*——从零构建 TCP/UDP 协议栈，支持本机 loopback 及网络 web 访问。打通“socket -> onps协议栈 -> VirtIO-Net”完整链路。iperf 和 netperf 四组合均可运行，可正常访问 web 服务。

- *交互式 BusyBox ash*——新增 `make shell r/l` 入口，使用独立 ext4 rootfs 镜像进入 BusyBox ash 交互终端。打通了 UART→控制台行规程→device_file→fd 0→BusyBox 的完整输入链路，支持 `pwd`、`ls`、`cat`、脚本执行和正常 `exit`。

- *动态 ELF 与 shebang 执行链*——支持 `PT_INTERP` 动态链接器路径读取与 musl/glibc loader 重写，按 ELF `p_align` 对齐装载（含 LoongArch 的 16KB 对齐要求）。支持 `#!` shebang 解释器递归和参数重写，使脚本文件可直接执行。

- *系统调用与 Linux ABI 完善*——累计新增或正式接入 19 个 syscall，包括 `clone3`、`execveat`、`personality`、`setpriority`/`getpriority`、`sendmmsg`/`recvmmsg`、`fanotify`（`fanotify_init`、`fanotify_mark`）、`epoll_pwait2`、`memfd_create`/`memfd_secret`、`eventfd2`、`name_to_handle_at`、`open_tree` 等。同时修复了大量已有 syscall 的边界语义和 errno 返回值。

- *文件系统增强*——实现 bind mount、open_tree 挂载视图、虚拟文件 `/proc` 增强、`splice`/`sendfile` 零拷贝数据搬运、`fcntl` 文件锁修复、flock 完善以及 loop 设备支持。

- *LoongArch 稳定性提升*——修复 ECODE 中断判定、TLB 管理、LL/SC 原子操作、trapframe 映射和用户态返回窗口寄存器恢复，使 LoongArch 侧可运行 pthread 多线程程序、libcbench 性能测试和 lmbench 微基准。

- *评测与工程体系*——建立四组合 scoreboard，提供 LTP runner/parser/ranker 工具链，日志统一输出到 `logs/run/`。所有评测结果可追踪、可复现。

- *内存与启动优化*——128 MiB 低内存适配、DTB 动态物理内存探测、LoongArch Split Heap 设计、execve 装载路径缓存优化，显著提升批量测例的启动速度。

== 项目目录结构

#figure(
  block(width: 100%, inset: 6pt, stroke: 0.5pt, radius: 4pt)[
    #set par(justify: false)
    #show raw.where(block: true): text.with(size: 7.2pt)
    ```text
    F7LY-OS
    ├── Makefile                         构建入口
    ├── AGENTS.md / agent_docs/           Agent 规则、架构与调试文档
    ├── scoreboard/ / plan_docs/          评测进度与任务计划
    ├── docs/report-src/                  Typst 报告源码
    ├── kernel/
    │   ├── boot/ hal/ trap/              启动、硬件抽象与异常处理
    │   ├── devs/                         UART、控制台、virtio 等设备
    │   ├── mem/ proc/ shm/ tm/           内存、进程、共享内存、时间
    │   ├── fs/vfs/ lwext4/ fat32/        VFS、ext4、FAT32
    │   ├── net/                          loopback + ONPS/VirtIO-Net
    │   ├── sys/                          Linux ABI 系统调用层
    │   └── libs/ link/                   基础库与链接脚本
    ├── user/app/ user_lib/ syscall_lib/  用户态入口、测试调度与 syscall 封装
    ├── user/deps/                        musl/glibc 工具链依赖
    ├── images/ busybox/                  QEMU 镜像与 BusyBox 产物
    ├── build/ logs/                      构建产物与运行日志
    ├── scripts/ tools/                   辅助脚本与工具
    └── thirdparty/EASTL/                 内核容器库
    ```
  ],
  caption: [F7LY-OS 项目目录结构],
) <code-project-structure>

== 2025 与 2026 能力对照

#figure(
  text(size: 8.8pt)[
  #set par(justify: false)
  #table(
    columns: (1.8cm, 3.0cm, 6.2cm, 1.5cm),
    align: (left, left, left, left),
    table.header(
      [*领域*], [*2025 文档状态*], [*2026 当前状态*], [*变化类型*]
    ),
    [C++ 环境], [宣称支持 C++ 异常], [C++23 freestanding；禁用异常与 RTTI], [纠正],
    [内存发现], [固定内存布局为主], [DTB 动态物理内存探测；双架构 Split Heap], [改进],
    [用户地址空间], [初步进程内存管理器], [三层 VMA 架构；统一 mmap、shm、brk、栈生命周期], [重构],
    [块 I/O], [架构独立驱动], [统一 virtio-blk 队列；priority-borrow I/O 调度], [新增],
    [网络], [BSD socket 协议栈框架], [loopback TCP/UDP/AF_UNIX 数据面；ONPS/VirtIO-Net 出网；iperf/netperf 验证], [改进],
    [进程管理], [基础 fork/clone/exec/wait], [线程支持；futex、robust futex、CLEARTID；clone3；POSIX timer；epoll], [改进],
    [文件系统], [C++ 文件系统 + FAT32], [lwext4 ext4 根文件系统；VFS 统一接口；FAT32 数据盘；bind mount；loop 设备], [新增],
    [系统调用], [约 60 个], [243 个；含 clone3、fanotify、memfd、splice 等], [新增],
    [用户入口], [自动连续测例 initcode], [自动连续测例；BusyBox ash shell；apk 包管理器], [新增],
    [评测体系], [分散日志], [四组合 scoreboard；LTP runner/parser/ranker；可复现实验], [新增],
    [双架构], [RISC-V 为主，LoongArch 基础], [双架构对等；LoongArch 可运行 pthread、libcbench、lmbench], [改进],
    [硬件平台], [QEMU virt], [QEMU virt；VisionFive 2；LS3A5000/LS2k1000 启动验证], [新增],
  )
  ],
  caption: [2025 与 2026 能力对照表],
) <tab:overview-capability-comparison>

= 01 整体架构

== 项目定位与决赛目标

F7LY-OS 是一个基于 xv6 思路继续扩展的教学 / 比赛内核。项目保留了 xv6 清晰的进程、地址空间、文件描述符和系统调用分层方式，但实现已经面向更接近 Linux 的用户态环境重建：内核使用 C++23 freestanding 编写，禁用异常和 RTTI，面向 RISC-V 与 LoongArch 两种架构提供统一的 Linux ABI。

决赛阶段把系统推进到可以承载真实 glibc rootfs、CAgent、BuildStorm、8 核、8 GiB 内存和大量 Rust / Cargo 编译压力的状态。这个目标同时约束了四个方向：用户态必须能够执行动态 ELF、shebang 脚本和 glibc 程序；内核必须在文件、进程、内存、IPC、时间、网络和 `/proc` 视图上返回可继续使用的 Linux 语义；多核路径必须处理跨核调度、futex、TLB shootdown 和对象回收；文件系统与块设备路径必须支撑大量源码和中间产物的并发读写。

CAgent 主要验证动态程序启动、命令执行、系统信息、网络状态和简单文件系统操作；BuildStorm 则在 8 核 / 8 GiB QEMU 中运行 Rust 工具链和数百 crate 的冷构建，持续放大调度扫描、缺页、COW、futex、ext4、页缓存、VMA 长度和大文件偏移等系统级问题。

为适应这一目标，F7LY-OS 在决赛阶段形成了更明确的系统边界：架构机制只处理入口、trap、页表 / TLB 和上下文切换；平台画像负责选择板级资源、链接脚本和设备后端；通用内核服务维护进程、内存、文件、IPC、时间和网络对象；Linux ABI 层把这些状态以 glibc、BusyBox、LTP、CAgent 和 BuildStorm 可理解的形式暴露给用户态。后续章节中的 Linux 语义、多核、网络和性能优化，都是在这一整体架构上继续收口。

== 参考项目与复用边界

F7LY-OS 不是从零实现全部组件的孤立内核。项目基线来自 2025 年武汉大学 F7LY 的延续仓库，并在此基础上继续推进双架构、Linux ABI 和评测适配。内存管理中复用了 LibAllocator 与 Buddy 的基本思路，内核容器使用 EASTL 适配 freestanding 环境，ext4 文件系统建立在 lwext4 之上，网络协议栈以 Open-NPStack 为基础；在实板支持和性能方向上，项目还参考了 2023 年华中科技大学 AVX 的 sdcard 驱动经验，以及 Starry-OS 在自编译、I/O 调度和性能优化上的横向经验。


F7LY-OS 在复用基础库和协议栈的同时，补齐比赛了所需的 Linux ABI 语义、双架构平台边界、多核并发不变量和性能证据。

== 当前能力总览

当前 F7LY-OS 已经具备 RISC-V 与 LoongArch 双架构启动能力，支持架构入口、trap、页表、TLB、上下文切换和 SMP 初始化。启动路径通过固件传入的 DTB 建立 `BootInfo`，公共启动流程再初始化内存、中断、块设备、VFS、次核和调度器。平台画像覆盖 RISC-V QEMU、LoongArch QEMU、VisionFive2 与 2K1000，块设备和网络设备通过平台 backend 接入 VirtIO、DWMMC / SD、AHCI、GMAC 等具体驱动。

用户态执行方面，内核支持动态 ELF 装载、`PT_INTERP` 处理、musl / glibc 动态链接器路径重写和 shebang 脚本入口。进程与线程路径支持 fork、exec、`clone` / `clone3`、线程组、`CLONE_VM`、`CLONE_FILES`、`CLONE_SIGHAND`、`CLONE_CHILD_CLEARTID`、futex、robust list、信号、进程组、wait 和 exit 回收。initcode 不再只是交互 shell 的入口，而是可以直接运行回归套件，并在结束后通过统一 `shutdown()` 路径退出评测。

内存系统已经从简单堆和页表管理扩展到面向 Linux 映射语义的地址空间模型。物理页由 Buddy 管理，用户地址空间由 `ProcessMemoryManager` 持有，`VmArea` 描述映射区间，`VMASpace` 负责拆分、合并、gap 查找和 Maple Tree 索引，`VmObject` 负责匿名页、文件页和 SysV SHM 后端。缺页、COW、`mmap`、`mprotect`、`munmap`、`mremap`、memfd、共享内存和文件页缓存都沿这条链路处理。同时，针对 LoongArch 内层 QEMU 和大文件访问暴露的问题，进一步把 VMA 长度、文件偏移和后备缺页路径收敛到 64 位语义，避免 8 GiB 预留和大偏移文件映射被 32 位截断。

文件、事件和网络方面，VFS 接入 ext4、虚拟文件、设备文件、pipe / FIFO、epoll、eventfd、timerfd 和 socket。ext4 路径支持真实挂载统计、目录遍历、文件读写、rename / link / unlink、fsync / fallocate、文件页缓存和块缓存回收；事件路径支持 poll / epoll、fd 化计数器、定时器和信号交互；网络路径支持 BSD socket、AF_INET / AF_UNIX、本机 loopback 快路径、Open-NPStack TCP / UDP / ICMP / IPv4 处理，以及由平台画像选择的 VirtIO-net 或 GMAC 后端。`/proc/cpuinfo`、`/sys/devices/system/cpu`、`sysinfo`、`/proc/uptime`、`/proc/version` 和 `/proc/net/tcp{,6}` 等接口已经从固定填充值推进到由真实内核状态生成。

验证层面，CAgent 的 10 项测试要求动态程序、时间、CPU / kernel 信息、网络状态和文件系统链路连续工作；BuildStorm 要求双架构 glibc rootfs 在多核大内存下完成 Rust / Cargo 冷构建。双架构 CAgent 连续轮次通过，BuildStorm 从只能启动工具链推进到双架构完整冷构建和后续性能收口，同时保留 Docker harness 与重复长测作为外部验收边界。

== 分层内核架构

我们将“内核”拆成一条自上而下的责任链。最上层是用户态程序，包括 initcode、BusyBox、CAgent、BuildStorm、LTP 以及普通的 glibc / musl / Rust 用户程序；它们只感知 Linux 风格的文件描述符、进程、信号、内存映射和 `/proc` 视图。

用户态之下是 Linux ABI 层。它的职责不是提供业务逻辑，而是把架构寄存器约定翻译成统一 syscall 参数、把内核对象状态翻译成 Linux 预期的返回值和 `errno`，并处理用户指针、结构体布局、字节序和权限检查。`syscall_handler.cc`、`user/syscall_lib/` 和相关的 trap 入口共同构成这一层，保证同一套用户程序在 RISC-V 和 LoongArch 上看到一致的调用契约。

再往下是通用内核服务层。进程、线程、调度、虚拟内存、VFS/ext4、IPC、时间和网络协议栈都在这一层。它们维护的是对象生命周期和并发不变量，例如 PCB 的引用关系、`VmArea` 的拆分合并、文件页缓存的回收、等待队列的唤醒顺序和 socket 状态的可观察性。服务层通过很小的 backend 接口使用平台能力，而不是直接依赖某块板子的寄存器地址。

平台画像层位于服务层和具体硬件之间。`mk/platform/` 决定一张完整画像该使用哪套架构、哪组板级资源、哪份链接脚本、哪组设备后端和哪种启动模式；`kernel/platform/<arch>/<board>/` 则把这些选择落成代码。这样一来，QEMU、VisionFive2 和 2K1000 只是同一套内核的不同画像，不再由通用代码里的 `BOARD` 分支互相污染。

最底层是架构机制层与设备层。架构层只处理入口汇编、CSR / 控制寄存器、trapframe、页表 / TLB 和上下文切换；设备层只处理中断控制器、串口、块设备、网卡、RTC 和 DMA 队列等硬件协议。两层之间通过 `BootInfo`、IRQ registry、console backend、clock backend、block backend 和 net backend 这些窄接口连接。这个结构的核心约束是：架构代码不保存板级地址，通用服务不猜测具体驱动，平台画像只负责组合，不重新实现通用逻辑。


#figure(
  image("fig/分层内核架构.png", width: 100%),
  caption: [F7LY-OS 2026 年整体架构图],
) <fig:overview-layered-architecture>

== 架构重建：平台画像与编译解耦

旧的构建方式把 `ARCH`、`BOARD`、initcode、链接脚本和驱动选择揉在同一套大 Makefile 里，通用代码很容易因为一个板子的差异被迫长出额外分支。决赛阶段的重建把“构建什么机器”和“如何编译内核”拆开：`make <动作> PROFILE=<完整平台画像> MODE=<启动模式>` 成为统一入口，`PROFILE` 一次性描述架构、板级目录、链接脚本、引导方式和设备组合，`MODE` 只区分 evaluation 和 shell 这类启动入口。

平台画像还明确了一个更重要的边界：一块板子只能对应一套活跃实现路径。RISC-V QEMU、LoongArch QEMU、VisionFive2 和 2K1000 各自拥有独立的 `kernel/platform/<arch>/<board>/` 目录，块设备、网络、console、IRQ、RTC 和电源管理都通过各自 backend 提供。这样做避免了通用代码里继续堆叠 `BOARD_*` 条件，也避免了不同板子的寄存器地址、链路脚本或中断控制器配置互相串入。

这一重建在运行时同样成立。`BootInfo` 只携带固件明确交给内核的事实，例如 DTB 物理地址、启动 CPU 和基础资源；DTB 决定 CPU 与内存，平台画像决定板级固定资源和驱动组合，设备驱动决定运行时状态。三者各司其职后，内核在切换到双架构、8 核、8 GiB 或不同实板时，只需要更换画像或固件输入，而不需要重新设计公共路径。也正因为如此，报告中的“架构重建”不是泛泛地说模块拆分，而是要把编译期、启动期和运行期的边界全部讲清楚。
#figure(
  image("fig/构建画像与编译解耦.png", width: 100%),
  caption: [构建画像与编译解耦],
) <fig:overview-profile-build>

== DTB 驱动的运行时硬件发现

平台画像解决的是“这份内核为哪类机器构建”，DTB 则解决“这次启动时真实硬件长什么样”。本阶段把运行时硬件发现收敛为一条原则：同一个硬件事实只保留一个权威来源。CPU / 内存来自 DTB，板上固定资源来自平台画像，设备队列、缓存和连接状态归设备驱动维护。

=== 固件输入

启动入口只接收固件传入的 DTB 物理地址，并将它和启动 CPU 标识一起转换为统一的 `BootInfo{boot_cpu_hwid, device_tree_paddr}`。RISC-V 与 LoongArch 的架构入口负责早期寄存器、栈、页表和 C++ 运行时准备，但不再把 CPU 数量、RAM 上限或板级设备地址写进公共初始化路径。

这个边界让 QEMU 与实板共用同一套启动 ABI。对于 VisionFive2，U-Boot 通过标准 Linux Image 启动约定传入 hartid 和 DTB；对于 QEMU 画像，固件同样提供 DTB 入口。公共内核只消费 `BootInfo`，不扫描内存猜测 RAM，也不在通用 `memlayout` 中保留 UART、PLIC、PCI 或 GMAC 等板级地址。

=== 统一解析

`DtbManager` 负责集中解析扁平设备树，将 memory / reserved-memory、memreserve、CPU hart、timebase、PLIC context、initrd、网卡 MAC 等信息转换为内核内部可以使用的事实集合。DTB 缺失、内存节点不可用或固件没有按约定传入参数时，内核应显式失败，而不是退回另一块机器的固定地址或固定内存上限。

这样做避免了多个模块各自解析或猜测硬件状态。CPU 拓扑、RAM 区间和保留区只在 DTB 管理路径中形成一次；板级固定资源仍由当前 `PROFILE` 选中的平台画像提供；设备驱动只接收已经类型化的资源和后端接口，不再维护第二份板级地址表。

=== 子系统消费

解析后的硬件事实由各个子系统按职责消费。PMM 根据 DTB 中的 memory 区间建立可分配 RAM，并排除 DTB 本身、reserved-memory、memreserve 和 initrd；Buddy 元数据、页引用计数和后续 clean page 回收都随实际 RAM 规模初始化。SMP 只启动 DTB 声明的 CPU，并把 possible / online CPU mask 作为调度、affinity、`getcpu`、`/proc/cpuinfo` 与 `/sys/devices/system/cpu` 的共同来源。

设备和平台路径则继续通过 backend 解耦。中断路径根据 DTB 或平台画像建立 PLIC context、ExtIOI / LIOINTC 等控制器视图；块设备和网络路径只看到 block backend 与 net backend；VFS 和网络栈不需要知道底层是 VirtIO、DWMMC / SD、AHCI 还是 GMAC。

#figure(
  image("fig/内核地址空间.png", width: 100%),
  caption: [内核地址空间],
) <fig:overview-kernel-address-space>

== VMA 记录重构：索引与后端解耦

Linux 用户态的大量行为最终都会落到地址空间管理上。BuildStorm 中的 Rust 工具链会频繁执行 `mmap`、`mprotect`、`munmap`、`mremap`、文件映射、匿名映射、COW 和多线程共享地址空间；LoongArch 内层 QEMU 还会预留 8 GiB guest RAM，并用大范围 `PROT_NONE`、`MAP_FIXED`、首尾触页和中段权限修改反复验证 VMA 语义。旧路径如果在 syscall、缺页、fork 和文件页缓存之间维护多份状态，就很容易出现区间找不到、权限不一致、页偏移截断或回收顺序错误。

决赛阶段将 VMA 语义拆成三个对象：

`VmArea` 只描述一个虚拟区间本身，包括起止地址、权限、映射类型、页偏移、增长策略和后端引用；

`VMASpace` 是地址空间的索引与变更入口，负责创建、拆分、合并、gap 查找、Maple Tree 索引重建以及和 `ProcessMemoryManager` 的锁顺序配合；

`VmObject` 是实际页来源，分别承担匿名页、文件页缓存和 SysV SHM 共享页的 `prepare_page` 语义。这样，syscall 层不再直接决定缺页时应该读文件、分配匿名页还是附着共享内存，而是把决策交给对应后端。

缺页路径因此收敛为一条稳定链路：trap 层识别用户态 page fault，地址空间通过 `VMASpace` 找到覆盖 fault 地址的 `VmArea`，检查权限和映射类型后调用 `VmObject` 准备物理页，页表层安装 PTE，再由架构层完成本地或跨核 TLB 一致性维护。普通 `read`、`mmap`、`exec` 可以共享文件页缓存，`MAP_PRIVATE` 写入通过 COW 或 private overlay 私有化，SysV SHM 则通过共享后端保持多进程可见性。

这个重构直接服务于决赛暴露的两类问题。第一类是并发和生命周期问题：`CLONE_VM` 线程共享同一个 mm，`fork` 需要复制或共享对应后端，`exec` 成功后要替换地址空间，退出路径要等引用归零后才能回收页表、ASID、trapframe 和 VMA 后端。第二类是大内存和大文件问题：VMA 长度、文件偏移、页偏移和后备缺页路径必须使用 64 位语义，否则 8 GiB 预留、大文件映射和 LoongArch 内层 QEMU 都会被 32 位截断破坏。

从整体架构看，VMA 重构的价值不只是让 `mmap` 更快，而是把内存语义从散落的条件分支变成可组合的对象模型。后续文件页缓存、COW、共享内存、TLB shootdown、地址空间 teardown 和性能优化，都可以沿 `VMASpace -> VmObject -> 页表` 这条路径讨论，而不必为每个 syscall 单独解释一套状态机。
#figure(
  image("fig/vma.png", width: 100%),
  caption: [vma],
) <fig:overview-vma>

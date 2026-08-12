= F7LY-OS 内核概述

== 决赛阶段的目标与范围

与初赛主要面向基础功能回归的环境相比，决赛阶段需要处理更多 CPU 带来的调度和锁竞争，8 GiB 内存和大量映射带来的页表、回收与缓存压力，以及 glibc、动态 ELF 和 Rust 工具链对 Linux ABI 细节的依赖。

== 本阶段的三条优化主线

第一条主线是扩大内核的承载范围。启动路径从固定机器假设改为读取 DTB 中的 CPU、RAM、reserved-memory 和 initrd 信息；物理内存管理、Buddy 元数据、引用计数、COW 和 TLB 更新随实际资源规模调整；调度器和进程线程管理则围绕 online CPU、runnable 任务和共享地址空间重新检查并发不变量。

第二条主线是建立架构、平台和设备的分层边界。RISC-V/LoongArch 的入口、trap、页表和 TLB 代码只处理架构机制；`kernel/platform` 和 `mk/platform` 负责选择板级资源与驱动组合；VirtIO、GMAC、AHCI、PLIC、ExtIOI 等设备代码只处理寄存器、DMA、队列和中断。通用内核服务通过 `BootInfo`、IRQ registry、clock/RTC、block backend 和 net backend 等小接口使用这些能力。

第三条主线是补齐 Linux 语义闭环。系统调用层统一处理寄存器参数、用户内存 copy、Linux errno 和 ABI 结构体布局，进程、内存、文件、IPC 和网络模块负责实际状态变化与生命周期；`/proc`、`statfs`、`sysinfo`、`getcpu` 和 `/proc/net/tcp{,6}` 等接口再把真实状态返回给用户程序。这样，CAgent 或 LTP 看到的不只是一个返回 0 的函数，而是可继续使用、可查询、可回收的系统状态。

== 当前系统能力总览

F7LY-OS 是一个面向 Linux ABI 的 C++23 freestanding 内核，支持 RISC-V 与 LoongArch 两种架构。内核提供多核启动、SMP 调度、动态 ELF 装载、进程与线程、`clone/clone3`、futex、信号、mmap/COW、SysV SHM、ext4/VFS、pipe/FIFO、epoll/eventfd、POSIX timer、socket 以及 TCP/UDP loopback 等能力。用户态通过 initcode 启动回归入口，shell 画像则用于交互式运行 BusyBox、glibc 和 Rust 工具链程序。

CAgent 从动态程序启动到文件、时间和网络接口的连续执行；BuildStorm 在 8 核、8 GiB QEMU 中从 toolchain/minibuild 进入并行 crate 编译，并对调度、futex、页表、ext4 和 exec 路径施加组合压力。

== 2026 年整体分层架构

=== 用户态层

用户态包括 initcode、BusyBox、CAgent、Rust/Cargo 工具链和 LTP 回归程序。它们通过 Linux 风格的系统调用、VFS 文件描述符和 `/proc` 文件与内核交互。

=== 系统调用与 Linux ABI 层

trap 入口把架构寄存器约定转换为统一 syscall 参数，handler 负责编号分发、用户指针检查、copyin/copyout 和错误码转换；进程、内存、文件、IPC、时间和网络模块负责具体状态。

=== 内核服务层

进程与线程、调度、虚拟内存、VFS/ext4、IPC、时间和网络协议栈属于通用服务。它们维护对象引用、锁顺序、等待队列、页表和文件状态，并通过小型 backend 接口调用平台能力。

=== 基础设施与平台层

内存分配器、页表管理、IRQ registry、定时器、console、block/net backend 构成基础设施；`kernel/platform/<arch>/<board>/` 与 `mk/platform/` 负责把这些接口组合到具体 QEMU 或实体板。平台层提供资源和驱动选择，通用服务不再复制板级分支。

=== 架构与设备层

架构层处理入口汇编、CSR/控制寄存器、trapframe、TLB 和上下文切换；设备层处理 PLIC、LIOINTC、ExtIOI、VirtIO MMIO/PCI、GMAC/JH7110、AHCI 等硬件协议。两层通过明确的初始化和收发接口向上提供能力。

== 小结

决赛阶段的 F7LY-OS 架构机制提供执行基础，平台组合提供设备资源，通用内核服务维护对象和并发语义，Linux ABI 将结果交给用户程序。第 2 至第 9 章将在这一坐标系下说明各模块的具体优化。

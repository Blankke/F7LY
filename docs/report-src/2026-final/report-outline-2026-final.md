# F7LY-OS 2026 决赛阶段优化报告写作大纲

> 写作范围：2026 年 7 月 12 日至当前提交。本文档沿用初赛报告的章节顺序，只规划本阶段新增、重构、优化和验证内容；初赛已有能力不再重复展开。
>
> 参考资料：`plan_docs/final_2026_support_gap_plan.md`、`docs/report-src/2026-final/架构设计.md`、本阶段 Git 提交记录及对应测试日志。

## 一、写作主线

1. **从以单核和功能回归为主的验证环境，扩展到面向 8 核、8 GiB 和大规模并发编译的决赛压力环境**：支持 `-smp 8 -m 8G`，覆盖 CAgent 与 BuildStorm 的真实 glibc/Rust 工作负载。
2. **对原有架构代码、板级资源和设备驱动的混合边界进行重构，建立“架构机制—平台组合—设备驱动”的分层结构**：统一 BootInfo、IRQ、console、clock/RTC、block、network 等边界，支持 RISC-V/LoongArch 及 QEMU/实板画像。
3. **从“系统调用能够被调用”转向“参数、状态变化、错误码、并发行为、资源回收和用户可观察结果均符合 Linux ABI 语义”**：补齐真实 CPU、时间、`sysinfo`、`statfs`、`/proc/net/tcp` 等观测接口，修复线程、futex、epoll、文件一致性和失败回滚。
4. **从能运行转向可诊断、可优化、可复现**：建立性能诊断框架、短测脚本和四组合构建入口，记录定向回归结果，并如实描述不同测试范围的证据。

## 二、按初赛框架映射的章节安排

### 第 1 章 F7LY-OS 内核概述

- 说明决赛阶段目标、评测约束和与初赛的差异：双架构、glibc、8 核、8 GiB、CAgent/BuildStorm。
- 给出 7 月 12 日以来优化总览：平台重构、SMP、8G 内存、Linux 语义、并发稳定性、性能观测。
- 更新系统分层图和目录结构，突出 `kernel/platform`、`kernel/hal`、`mk/platform`、脚本与工具链。
- 增加“优化范围与证据范围”：介绍 CAgent 连续通过结果，以及 BuildStorm 从启动、工具链到并行编译阶段的实际观测。

### 第 2 章 机器启动

- 统一 `kernel_init` 和 `BootInfo` 启动入口，架构入口只负责寄存器、DTB 和启动参数。
- 介绍 RISC-V/LoongArch 的 secondary CPU 启动、每核 trap/timer/IRQ 初始化和 scheduler 放行屏障。
- 说明 DTB 驱动的 RAM、CPU、reserved-memory、initrd 发现，替代固定内存上限。
- 介绍新的 `PROFILE`/`MODE` 构建系统、evaluation/shell 双入口、决赛磁盘镜像准备流程。
- 证据：双架构 `-smp 8 -m 8G` 启动日志、四种 profile 构建结果、QEMU 参数与镜像校验记录。

### 第 3 章 中断管理器

- IRQ 注册表与 backend 解耦：设备先登记 source/owner，控制器只负责 claim-dispatch-complete。
- RISC-V PLIC 按 hart 初始化；LoongArch 使用 CPU 槽位、IPI 和平台中断 backend。
- 介绍 SMP 语义化唤醒 IPI、定时器中断和跨核通知路径；TLB shootdown 的地址空间语义放入第四章展开。
- 说明 LoongArch trap/TLB、RISC-V trap 往返及用户 trapframe 隔离的决赛修复。
- 证据：多核 stress-ng、IPI/timer/epoll 定向回归及无 panic 日志。

### 第 4 章 内存管理

- 8G PMM 改造：managed pages 动态 refcount、Buddy 元数据动态存储、heap/shm/page cache 地址布局。
- `VMASpace/VMObject` 收口：mmap、exec、用户栈、共享内存、COW 和缺页统一后端；说明 CLONE_VM 共享 mm。
- 跨核页表更新和 ASID 生命周期：活跃 mm CPU 定向 TLB shootdown、线程共享 ASID、TLB 批处理；说明 IPI 只是通知手段，核心仍是地址空间一致性。
- mmap/brk/mprotect/mremap/madvise、页回收、文件页缓存和 teardown 并发修复。
- 证据：8G 启动与页数、mmapstress03/04/05、shm/COW/TLB、clone/exec 失败回滚专项。

### 第 5 章 进程与线程管理

- SMP scheduler：online CPU 集合、每核 runnable/运行槽位、affinity、sticky home CPU 和 O(NUMCPU) 初始选核。
- clone/clone3 线程语义：共享 mm/files/sighand、TLS、tid/clear_tid、线程组 exec/exit、失败路径资源回滚。
- 修复线程退出最后 mm 引用归还、PCB 锁生命周期、RUNNABLE 子任务回收和 wait/reparent 唤醒。
- 说明 BuildStorm 中 cargo/rustc 多进程、多线程和 proc-macro 工作负载对应的内核路径。
- 证据：clone301/302/303、futex、24×8 线程退出竞态、CAgent/BuildStorm 前段日志。

### 第 6 章 文件系统

- 平台 block backend 统一：VirtIO MMIO/PCI、AHCI 与公共块设备接口；真实设备 capacity 与分区处理。
- ext4/bcache 并发稳定性：可重入 FIFO mount lock、`Ext4MountGuard`、dirty list O(1)、写回和最后 close flush。
- 文件路径性能：正/负组件缓存、symlink 单遍解析、目录项缓存、relatime、root 路径 resolve 合并。
- 修复目录 cookie、O_TMPFILE、rename/unlink/link、pread/pwrite、fsync/fallocate、statfs/fstatfs 语义。
- 证据：CAgent 文件四项连续通过、ext4 并发短测、e2fsck、文件操作 LTP 小集合和 BuildStorm 性能探针。

### 第 7 章 进程间通信

- futex 锁序统一与原子 key 发布，消除 WAIT/WAKE ABBA 和丢唤醒窗口。
- 普通 sleep/wakeup 先按 channel 筛选候选，避免跨核扫描无关 PCB 造成锁反转。
- epoll 零超时无页分配、轮转游标、ET/LT 事件交付公平性；timer 与 epoll 快路径。
- POSIX timer 创建、归属、删除、overrun、进程退出清理和定时中断扫描优化。
- 证据：futex 四项、`epoll_wait04` 连续 10 次、POSIX timer 19 项定向回归。

### 第 8 章 系统调用

- 决赛阶段 Linux ABI 语义补全：`sysinfo`、`statfs/fstatfs`、`uname`、`getcpu`、affinity、时间接口。
- `/proc` 真实状态视图：CPU 拓扑、uptime/load、进程数、`/proc/version`、`/proc/net/tcp{,6}`。
- syscall 参数校验、用户内存 copy、错误码和结构体布局的一致性；强调不以绑定数量代替完成度。
- 介绍 syscall handler 组织、诊断接口与正式构建中的默认关闭策略。
- 证据：CAgent 10/10、LTP 78 项双架构定向回归、`/proc/f7ly/perf` ABI smoke。

### 第 9 章 网络系统模块

- ONPS -> 公共帧收发接口 -> 平台 net backend 的新分层，覆盖 VirtIO 与 GMAC/JH7110。
- `/proc/net/tcp/tcp6` 从 socket 生命周期登记生成真实 LISTEN/CONNECTING/ESTABLISHED 快照。
- SMP 下 socket 登记无固定容量上限、锁初始化和并发 close 修复；保持 TCP/UDP loopback 语义。
- 证据：CAgent network 连续通过、TCP/UDP 与网络状态定向测试、双架构构建日志。

### 第 10 章 总结和展望

- 汇总本阶段已完成优化与量化证据，按“架构能力、语义正确性、性能、可复现性”分类。
- 单列 CAgent 结论：RV/LA glibc 连续三轮 10/10。
- 单列 BuildStorm 结论：说明 8G/8 核启动、Rust toolchain/minibuild 和前段并行编译的实际结果，并区分短测、长测和 workflow 证据。
- 总结性能优化收益：调度扫描、路径缓存、bcache、COW/TLB、文件页缓存、syscall buffer 复用及性能诊断框架。
- 说明限制与后续工作：完整长压、内存回收峰值、ext4 长时间一致性和 Docker 双架构复现。

### 附录

- A：决赛阶段新增/修改/删除功能表（按提交或模块归类）。
- B：CAgent/BuildStorm 依赖的关键 syscall 与 Linux 语义变化表。
- C：四种 profile、QEMU 8G/8 vCPU、镜像和构建命令复现清单。
- D：定向回归、性能探针、GDB/日志证据索引及测试范围说明。
- E：平台能力矩阵（RISC-V/LoongArch × QEMU/VisionFive2/2K1000）。

## 三、正文写作约束

- 每节围绕“原问题—本阶段改动—设计取舍—验证结果”展开，避免重复初赛 API 介绍；未覆盖的内容只在必要处通过适用范围自然说明。
- 代码引用优先使用当前提交后的路径；截图和日志只引用可复现来源，不把诊断探针输出当作功能通过证据。
- 性能数据区分 guest 时间、宿主采样和吞吐，不把 QEMU TCG 结果表述为原生硬件性能。
- 不单独列出未完成任务或验收清单；对于不同测试范围，直接在对应结果叙述中说明测试条件和证据类型。
- 不为单项修复单独设置“本阶段修复”等小标题，修复原因、实现方式和结果直接融入所属技术主题的正文。

= 机器启动

本阶段收拢架构入口、平台资源和公共内核初始化之间的边界，使同一套内核能够在两种架构、两种 QEMU 画像和两块实际开发板上构建，并在决赛要求的 8 核、8 GiB 配置下启动。启动路径最终需要把本次机器的 CPU、内存和 DTB 信息准确交给公共内核，同时避免把某一块板卡的地址或设备假设泄漏到通用代码中。

== 统一启动约定

=== 架构入口只负责建立 BootInfo

RISC-V 和 LoongArch 仍然分别保留自己的 `entry.S`、早期栈和寄存器初始化代码，但架构入口不再直接初始化文件系统、网络或调度器。入口阶段只完成以下工作：

- 保存固件传入的启动 CPU 标识；
- 读取并保留设备树地址等启动参数；
- 建立能够运行 C++ 代码的栈和最小地址映射；
- 将信息转换为统一的 `BootInfo`，调用公共的 `boot::initialize_kernel(...)`。

公共入口随后按固定顺序初始化全局内核服务、当前 CPU 的 trap/timer、块设备和 VFS，最后创建第一个用户进程并放行 scheduler。这样，架构差异集中在入口和 HAL，公共启动流程不再按架构复制两份。

=== 启动信息的唯一来源

本阶段明确了不同硬件事实的归属：CPU 的寄存器机制放在架构层，板上固定的 UART/IRQ/MMIO 资源放在平台配置，本次启动发现的 CPU 与 RAM 数量来自 DTB，设备运行状态由具体驱动维护。公共 `memlayout` 不保存板级设备地址，也不在 DTB 解析失败后猜测另一块机器的内存上限。

== 双架构与多核启动

=== RISC-V：boot hart 与 secondary hart

RISC-V 由 OpenSBI 负责提供 boot hart，并通过 HSM 启动其他 hart。boot hart 完成全局 PMM、页表、VFS、设备和进程对象的初始化；secondary hart 只初始化自己的 CPU 槽位、trap、timer 和中断上下文，然后在全局屏障处等待。所有在线 hart 就绪后，公共启动流程才创建并运行用户任务。

PLIC 的初始化和 claim/complete 按当前 hart 进行，消除了只对 hart 0 配置中断的假设。用户线程共享地址空间时，每个 PCB 仍使用槽位专属的用户 trapframe 映射，避免多个 hart 返回用户态时互相覆盖现场。

=== LoongArch：CPU 槽位与 IPI

LoongArch 入口从固件参数建立 `BootInfo`，次级 CPU 通过 QEMU 提供的 mailbox/IPI 路径进入各自的 HAL 初始化。内核 CPU 槽位使用 CSR_CPUID 判定，不读取可能被用户 TLS 修改的寄存器。平台中断控制器、定时器和 TLB 失效操作均由当前 CPU 的 backend 完成，公共调度器只依赖统一的 online CPU 集合。

=== 启动屏障与调度放行

多核启动中最重要的不变量是“全局对象只初始化一次、每个 CPU 局部状态只初始化一次”。因此 boot hart 与 secondary hart 之间设置显式阶段屏障：全局内存和设备服务完成后，次级 CPU 才进入可调度状态；online 集合建立后，`/proc/cpuinfo`、affinity 和 `getcpu` 使用同一份 CPU 视图。8 核启动日志和 stress-ng 短测用于验证 CPU 没有重复初始化、非法索引或提前调度。

== DTB 驱动的资源发现

=== RAM、CPU 与保留区

初赛的固定内存上限无法覆盖决赛的 8 GiB QEMU 配置。本阶段由 DTB 解析 RAM 区间、CPU 节点、`reserved-memory` 和可选 initrd，并将结果转换为 typed 资源交给 PMM 和启动布局。物理页引用计数表和 Buddy 元数据按最终 managed pages 动态分配，不再受 1 GiB 固定数组或固定 Buddy 元数据页数限制。

PMM 在启动时显式切分内核镜像、页表、heap、共享内存和普通可分配页，避免把 DTB、initrd 或保留区再次加入空闲页。DTB 缺失或内存描述不可用时直接报告启动参数错误，而不是扫描未知地址或回退到其他画像的 `PHYSTOP`。

=== 设备资源转换

DTB 是启动输入，不是驱动的长期状态。平台层在初始化阶段读取设备树属性，将 UART、IRQ、MMIO、时钟和内存区域转换为平台 backend 所需的 typed 资源；驱动只消费这些资源，不再自行保存第二份板级地址。该边界同时适用于 RISC-V QEMU/VisionFive2 和 LoongArch QEMU/2K1000。

== 画像化构建与启动模式

=== PROFILE 组合入口

为避免 `ARCH`、`BOARD` 和驱动集合被任意组合，本阶段将完整机器画像作为唯一选择入口：

```text
make <动作> PROFILE=<架构-平台> MODE=<启动模式>
```

当前画像包括 `riscv-qemu`、`loongarch-qemu`、`riscv-visionfive2` 和 `loongarch-2k1000`。`mk/platform` 选择架构、平台目录、链接脚本和设备驱动；通用源码、架构源码和平台源码按画像进入构建。`build-config.stamp` 和 `kernel-sources.list` 记录配置及实际源文件，配置变化或源码增删会触发正确重建。

=== evaluation 与 shell

决赛评测使用 evaluation initcode，直接进入 CAgent/BuildStorm；人工交互使用 shell initcode，启动 BusyBox ash。`run` 固定 evaluation，`shell` 固定 shell，避免把调试入口混入正式测例。构建阶段不依赖磁盘，只有真正启动 QEMU 时才准备对应 rootfs。

== 决赛镜像与启动验证

`QEMU_DISK=final` 选择决赛完整 rootfs，`QEMU_MEM=8G QEMU_SMP=8` 模拟官方 BuildStorm 资源约束。镜像准备脚本优先使用人工放置且经过校验的决赛镜像，并保留原始 `images/` 资产不被运行过程覆盖；需要快照运行时使用临时副本。

VisionFive2 的内核镜像带有标准 RISC-V Linux Image v0.2 头，U-Boot 使用 `booti` 同时传入 kernel 和真实 DTB，启动寄存器遵循 `a0=hartid、a1=DTB` 约定。

本阶段完成了双架构四种画像的构建，以及 QEMU 8 核、8 GiB 的启动和定向回归；CAgent 获得了双架构连续通过结果，BuildStorm 则记录了工具链、minibuild 和前段并行编译过程。这些结果分别对应功能回归、资源启动和构建前段的测试范围。
== 本章小结

本阶段的启动优化把“架构能进入 C++”扩展为“机器资源可发现、多个 CPU 可协同、平台驱动可组合、评测和 shell 入口可复现”。后续章节中的 SMP 调度、跨核 TLB、8G 内存和设备并发，均建立在本章定义的统一启动约定和资源边界之上。

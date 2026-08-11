# 项目架构导航

## 文档概况

本文档给 agent 快速建立 F7LY 的架构地图。它说明各模块“负责什么、依赖什么、改动时先看哪里”，不追逐每个函数细节。平台重构的设计理由、取舍和接入规则见 [platform_refactor_design.md](platform_refactor_design.md)；第一次上 VisionFive2 或 2K1000LA 实机时读 [board_bringup_beginner.md](board_bringup_beginner.md)；需要命令和运行方法时读 `agent_docs/development_debugging.md`；需要评测进度时读 `agent_docs/scoreboard.md`。

## 总体分层

F7LY 的依赖方向是：

1. 架构机制：`kernel/boot/<arch>/`、`kernel/hal/<arch>/`、`kernel/trap/<arch>/`，只处理入口寄存器、CSR、页表/TLB、trap 和上下文切换。
2. 平台组合：`mk/platform/` 与 `kernel/platform/<arch>/<board>/`，在编译期选择当前机器的资源、控制器后端和设备驱动。
3. 设备与控制器：`kernel/devs/`、`kernel/fs/drivers/`、`kernel/net/drivers/`、各架构中断控制器，只处理寄存器、DMA 和设备协议。
4. 通用内核子系统：内存、进程、调度、时间、VFS、网络栈和 Linux ABI。
5. 用户态与回归：`user/`、`scoreboard/`。

核心规则是“架构机制不保存板级地址，通用子系统不识别具体驱动，平台画像只组合实现”。QEMU 不是架构：RISC-V QEMU 与 LoongArch QEMU 是两套独立画像。新增机器时不能继续往通用代码里增加 `BOARD_*` 分支。

当前公共接口刻意保持很小：`BootInfo`、console backend、IRQ registry/backend、clock/RTC backend、block backend、net backend 和 `NetworkConfig`。不要把它们重新合成一个巨型 `PlatformManager`。

## 顶层目录职责

- `kernel/`：内核主体。
- `user/`：用户态 initcode、系统调用封装、回归测试入口。
- `thirdparty/EASTL/`：内核使用的 EASTL 容器库。
- `busybox/`：预置 BusyBox 二进制，按架构和 libc 区分。
- `images/`：本地运行镜像、sdcard 备份等大文件资产。
- `scripts/`：挂载、镜像恢复、宿主机辅助脚本。
- `debug/gdb/`：按架构拆分的 GDB 调试配置。
- `tools/ltp/`：LTP 输出解析、排名、历史 scoreboard 工具。
- `scoreboard/`：当前四组合 Markdown scoreboard。
- `ref/ltp/`：上游 LTP 参考源码，当前应 checkout 到 `20240524` tag。
- `ref/testsuits-for-oskernel/`：basic、busybox、lua、libcbench 等非 LTP 测例参考源码。
- `docs/archive/`、`docs/dev-notes/`：历史材料和排障记录；当前事实仍以源码和 Git 记录为准。

## 启动与架构入口

RISC-V：

- 入口：`kernel/boot/riscv/entry.S`
- 早期启动：`kernel/boot/riscv/start.cc`
- 架构适配入口：`kernel/boot/riscv/main.cc`
- trap：`kernel/trap/riscv/trap.cc`
- 页表：`kernel/mem/riscv/`
- virtio disk：`kernel/fs/drivers/riscv/virtio_disk2.cc`

LoongArch：

- 入口：`kernel/boot/loongarch/entry.S`
- 架构适配入口：`kernel/boot/loongarch/main.cc`
- trap：`kernel/trap/loongarch/trap.cc`
- TLB refill/uservec：`kernel/trap/loongarch/`
- 页表：`kernel/mem/loongarch/`
- VirtIO PCI 公共传输协议：`kernel/devs/virtio/`；块设备语义属于 `kernel/fs/drivers/loongarch/`，PCI host 的 ECAM 枚举、配置访问和 BAR 分配属于 `kernel/platform/loongarch/qemu/pci.cc`。旧的 `kernel/devs/loongarch/` 原型已删除，不能再建立第二套入口

两种架构最终都把 `BootInfo{boot_cpu_hwid, device_tree_paddr}` 交给 `kernel/boot/kernel_init.cc`。公共启动顺序只有一份：全局 trap/IRQ 控制器、内存与核心服务、当前 CPU trap/IRQ、块设备与 VFS、次核、调度器。不要在架构 `main.cc` 中复制这套顺序。

链接脚本保留 `.init_array`，入口在使用任何带构造函数的全局对象前统一完成 BSS 清零和 C++ 全局构造。启动栈单独放在 `.bss.stack`，不能被正在使用栈的 CPU 清零。

LoongArch 的敏感点是高地址内核映射、TLB refill、trapframe 动态映射、用户态 LL/SC 保留窗口。涉及 pthread、futex、信号、clone、mmap 时优先考虑这些交界。

## 平台画像与板级适配

当前可构建画像：

| 画像 | 组合目录 | 块设备 | 网络 | 中断控制器 |
| --- | --- | --- | --- | --- |
| `riscv-qemu` | `kernel/platform/riscv/qemu/` | VirtIO MMIO | VirtIO MMIO | PLIC |
| `riscv-visionfive2` | `kernel/platform/riscv/visionfive2/` | JH7110 DWMMC/SD | JH7110 GMAC1 | PLIC（DTB context） |
| `loongarch-qemu` | `kernel/platform/loongarch/qemu/` | VirtIO PCI | VirtIO PCI | LS7A PCH PIC + ExtIOI |
| `loongarch-2k1000` | `kernel/platform/loongarch/2k1000/` | AHCI | GMAC | LIOINTC |

每个 `mk/platform/<arch>-<board>.mk` 是唯一 composition root：它选择一个
平台目录、链接脚本，以及位于平台目录之外的确切设备驱动。中断控制器等
板级实现直接放在对应平台目录中，由目录归属完成正向选择。每项必需平台
能力只能有一条明确的活动实现路径；不能把多块板的驱动全部编进来再靠宏
或排除表让其中一部分失效。

LoongArch QEMU 的 PCI host 边界是 `kernel/platform/pci.hh`：驱动用 BDF 查找设备并按 function 内偏移访问配置空间，平台层按 generic ECAM 的 4 KiB/function 规则计算地址、分配并映射 32/64 位 memory BAR。VirtIO block/net 只解析自己的 capability 和队列协议，不能再拼 ECAM 地址、保存 `bus1/device1` 之类跨模块状态或各自实现 BAR 分配。

资源归属按类型区分，不能用一个“整机资源来源”标签概括：

- DTB 是启动 CPU、RAM、memreserve、`reserved-memory` 和可选 initrd 的权威输入。DTB 缺失或内存描述不可用时直接失败，不扫描 RAM、猜地址或退回另一台机器的内存上限。
- 现有已知机器的 UART、IRQ、MMIO 等设备资源集中在所选平台的 `platform_board_config.hh`，以 typed region/常量交给驱动；通用 `memlayout.hh` 不保存设备地址。
- 后续接入 DTB 设备发现时，应在平台层把 FDT 属性一次转换为同样的 typed 资源，再交给驱动。驱动不能自行保存第二份板级地址。
- 固定资源只能是明确的平台契约，禁止在 DTB 解析失败后静默启用固定 fallback。

VisionFive2 已按同一边界接入，而不是整体搬运旧分支的 `#ifdef`：`mk/platform/riscv-visionfive2.mk` 选择独立平台目录、链接脚本、DWMMC 和 GMAC；入口消费固件传入的 DTB；console、PLIC、clock、block、net 分别实现窄 backend。PLIC context 从 DTB 的 `interrupts-extended` 建表，不能假设 hartid 就是 context。SD 路径是“JH7110/DWMMC host -> SD 协议 -> block backend”，分区识别仍由公共块层负责。GMAC 首版为非一致性 DMA 的轮询实现，通过 JH7110 CCACHE 做显式 cache maintenance，不注册 IRQ78。

VisionFive2 的 raw Image 带标准 RISC-V Linux Image v0.2 头。U-Boot 必须用 `booti <kernel_addr> - <fdt_addr>` 建立 `a0=hartid、a1=DTB` 的标准启动 ABI；旧分支使用的 `go 0x40200000` 只会传 `argc/argv`，不再是有效启动入口。

## 内存管理

核心模块：

- 物理内存：`kernel/mem/physical_memory_manager.*`
- 内核堆：`kernel/mem/heap_memory_manager.*`
- 页表与映射：`kernel/mem/virtual_memory_manager.*` 和架构页表目录
- 用户地址空间：`kernel/proc/process_memory_manager.*`
- VMA/地址空间对象：`kernel/proc/vm_area.hh`、`kernel/proc/vma_space.*`、`kernel/proc/vma_maple_tree.*`
- VMA 元数据辅助层：`kernel/proc/vma_metadata_utils.*`
- 页后端对象：`kernel/proc/vm_object.*`
- 共享内存与 IPC 入口：`kernel/shm/shm_manager.*`

关键思路：

- PMM 用 buddy 管理页级物理页。
- HMM 在 PMM 切出的 heap 区域上做细粒度分配，全局 `new/delete` 走这里。
- `ProcessMemoryManager` 是用户地址空间权威所有者，统一管理 ELF 段、heap、mmap VMA、用户页表和引用计数。
- `VmArea` 是统一的虚拟区间元数据，旧 `vma/NVMA` 仍在兼容路径里存在，但新的权威后端字段已经放到 `object/page_offset/private_page_overlay` 上。
- `VMASpace` 用 `eastl::list<VmArea>` 持有区间对象，用 `VmaMapleTree` 做按地址索引，后续 `find/gap/split/merge/fault` 都应朝这层收口。
- `VmObject` 负责匿名页、文件页、共享页的实际后端；`VirtualMemoryManager::allocate_vma_page()` 现在已经支持优先走 `VmObject::prepare_page()` 安装页表。
- `vma_metadata_utils.*` 负责兼容旧 `NVMA` 槽位模型时的 snapshot / split / rollback / overlay owner 管理，当前 `fork`、`munmap`、`mprotect` 都依赖它避免把 `VmArea` 里的非平凡成员按 POD 处理。
- `CLONE_VM` 共享 mm；普通 fork 深拷贝 mm；exec 成功后替换 mm。
- `allocate_vma_page()` 是当前缺页/惰性补页统一入口，修改 mmap/munmap/mremap 时必须同时检查 trap 缺页、`copy_in/copy_out`、fork COW 和退出释放。

## 共享内存与对象后端

核心模块：

- SysV SHM 管理与系统调用入口：`kernel/shm/shm_manager.*`
- 统一页后端：`kernel/proc/vm_object.*`

关键思路：

- `ShmManager` 目前仍保留 SysV IPC 元数据和附件管理职责，但新的页后端抽象已经迁到 `VmObject` 体系，后续目标是不再让共享内存逻辑直接决定普通文件/匿名映射的物理页安装。
- 文件映射、匿名映射、共享映射最终都应收束为“`VmArea` 描述区间，`VmObject` 提供页源，页表层统一安装”的模型；排查共享内存 bug 时不要只看 `shm_manager.cc`，也要同时看 `allocate_vma_page()`、`clone_for_fork()` 和 `free_single_vma()`。

## 进程、线程与调度

核心模块：

- PCB：`kernel/proc/proc.hh`
- 进程池与生命周期：`kernel/proc/proc_manager.cc`
- 调度：`kernel/proc/scheduler.cc`
- 上下文切换：`kernel/proc/<arch>/swtch.S`
- futex：`kernel/proc/futex.cc`
- 信号：`kernel/proc/signal.cc`
- pipe/FIFO：`kernel/proc/pipe.cc`、`kernel/fs/vfs/fifo_manager.cc`

关键不变量：

- `Pcb::_lock` 保护单个 PCB 状态；`ProcessManager::_wait_lock` 保护父子等待和 reparent。
- `_pid` 是进程 ID，`_tid` 是线程 ID，`_tgid` 是线程组 ID。
- `CLONE_THREAD` 共享 TGID；`CLONE_VM` 共享 mm；`CLONE_FILES` 共享 fd 表；`CLONE_SIGHAND` 共享 signal handler 表。
- `CLONE_CHILD_CLEARTID` 和 `set_tid_address` 退出时写 4 字节 pid_t 零，并 futex wake，不要写 8 字节。
- `wait4()` wait status 使用 Linux 编码：正常退出 `(exit_code & 0xff) << 8`，信号退出低 7 位存信号。

## Linux ABI 与系统调用

核心模块：

- 编号：`kernel/sys/syscall_defs.hh`
- 声明：`kernel/sys/syscall_handler.hh`
- 分发与实现：`kernel/sys/syscall_handler.cc`
- 用户态封装：`user/syscall_lib/`

关键思路：

- 系统调用号大体沿用 asm-generic Linux 编号。
- `SyscallHandler::init()` 先把分发表填成默认 ENOSYS，再用 `BIND_SYSCALL(name)` 注册。
- syscall 参数从当前进程 trapframe 的 `a0..a5` 读取，syscall number 在 `a7`。
- 内核侧返回负 Linux errno，例如 `-EINVAL`、`-EFAULT`。
- 新增 syscall 需要同步 defs、handler 声明、handler 实现、绑定列表，必要时同步用户态 wrapper 和测例入口。
- `syscall_handler.cc` 很大，修改前先定位同类 syscall 的参数校验、用户拷贝、fd 检查和错误码风格。

## Trap、中断与用户态返回

设备中断统一走 `kernel/hal/irq.*`。设备初始化时先用 `register_handler()` 声明 source 的 owner；平台控制器只开放已经登记的 source。trap 只做 `claim -> dispatch -> complete`，不得再按 UART、块设备、网卡写硬编码分支。PCI 共享中断允许同一 source 注册有限个处理函数，处理函数必须自行确认设备状态。

RISC-V：

- 用户 syscall cause 为 8，`epc += 4` 后进 syscall handler。
- page fault cause 12/13/15 走 mmap 懒分配，失败发 SIGSEGV。
- timer tick 推进 ticks、唤醒 sleep、检查 POSIX timer 和 interval timer。

LoongArch：

- syscall ecode 为 `0xb`，`era += 4` 后进 syscall handler。
- page fault ecode `0x1..0x7`；地址错误 `0x8/0x9` 直接 SIGSEGV。
- `ESTAT` 同时包含异常编码和中断 pending，`devintr()` 必须先确认 ecode 为 0 再分发中断。
- 对“PTE 合法但 TLB 残留旧状态”的用户页，先按页失效 TLB 重试。
- `usertrapret()` 会按当前线程 trapframe 动态重映射 TRAPFRAME，并尽量减少 TLB 失效，避免破坏 LL/SC。

## 文件系统与文件对象

核心模块：

- VFS 工具：`kernel/fs/vfs/vfs_utils.cc`
- ext4 封装：`kernel/fs/vfs/vfs_ext4_ext.cc`、`kernel/fs/vfs/vfs_ext4_blockdev_ext.cc`
- lwext4：`kernel/fs/lwext4/`
- file 抽象：`kernel/fs/vfs/file/`
- 虚拟文件系统：`kernel/fs/vfs/virtual_fs.*`
- 块缓存：`kernel/fs/bio.cc`

关键思路：

- 平台 block backend 只暴露裸盘扇区能力；`platform_block.cc` 统一识别裸 ext4、MBR 和 GPT，并优先把选中的 ext4 区域映射成逻辑设备 0。找不到 ext4 时显式暴露整盘，交由文件系统策略判断 FAT 或 DTB initrd；驱动和 VFS 都不能保存自己的全局分区偏移副本。
- 固件通过 DTB 明确提供 ext4 initrd 时仍可作为根；没有 initrd 时直接使用平台主块设备，不猜测内存中的旧镜像。
- `filesystem_init()` 在第一个进程上下文中运行，因为挂载可能 sleep。
- 新式 `fs::file` 是主要 file 对象抽象，派生类包括 normal、directory、device、pipe、socket、virtual、fat32、epoll。
- old `struct file` 与 new `fs::file` 并存，修改 fd 路径前先确认实际对象类型。
- ext4 全局 `extlock` 允许同进程递归进入，用于串行化 lwext4 状态。
- `/proc`、`/etc`、`/dev` 下大量内容由 `VirtualFileSystem` 注册。

## ELF 装载与用户态回归

核心路径：

- exec：`ProcessManager::execve()`
- 用户入口：`user/app/initcode-rv.cc`、`user/app/initcode-la.cc`
- 回归调度：`user/user_lib/user_test.cc`
- 用户 syscall ABI：`user/syscall_lib/arch/<arch>/syscall_arch.hh`

exec 关键思路：

- 支持 shebang，重写成解释器路径加脚本路径。
- 支持 `PT_INTERP`，会重写 musl/glibc 动态链接器路径。
- 加载 ELF `PT_LOAD` 时按 `p_align` 对齐；LoongArch 16K 对齐段不能退化成 4K。
- 装载成功后关闭 CLOEXEC fd，替换 mm，设置 trapframe PC/SP。

回归关键思路：

- initcode 不是 shell，而是直接调用 `regression_suite_4d1444()`。
- `run_test()` 负责 fork、子进程 setpgid、execve、父进程 waitpid 和 PASS/FAIL 输出。
- 每个测例独立进程组，避免 LTP `kill(0, SIGKILL)` 误杀 init。
- 当前 scoreboard 记录磁盘测例结构和人工/agent 协作状态，不等同于运行日志。

## 修改时的定位规则

- 新开发板或设备组合：先新增/修改 `mk/platform/<arch>-<board>.mk` 与对应 `kernel/platform/` 目录；通用代码中出现新的 `BOARD_*` 通常表示边界放错。
- UART、IRQ、时钟、RTC、块设备、网卡差异：先看对应窄 backend；设备协议问题再进入具体驱动，不能从 VFS/网络栈反向选择硬件。
- DTB/内存问题：先核对固件传入地址、DTB memory/reserved 节点和 `BootInfo`，不要增加扫描或固定 RAM 兜底。
- syscall 语义问题：先看 `syscall_handler.cc`，再看 proc/fs/mem 具体后端。
- exec/动态链接问题：先看 `ProcessManager::execve()`、ELF auxv、loader 路径重写。
- mmap/缺页问题：同时看 syscall mmap、VMA 元数据、trap page fault、`copy_in/copy_out`。
- fork/clone/pthread 问题：同时看 clone flags、mm/file/sighand 共享、futex、signal、架构 usertrapret。
- 文件语义问题：先确认是虚拟文件、设备文件、普通 ext4 文件、pipe/socket 还是 old file 路径。
- LoongArch 独有问题：优先检查 TLB、DMWIN、PTE_MAT、trapframe 动态映射、LL/SC 相关补丁。

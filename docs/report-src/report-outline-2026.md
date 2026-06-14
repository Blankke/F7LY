# F7LY-OS 2026 年 OSCOMP 设计文档提纲

## 1. 文档定位

本报告不是重写 2025 年设计文档，而是在核对当前源码后，统一修订过时描述，并重点说明 2026 年新增、改进和删除的内容。

写作基线：

- 旧报告：`docs/archive/design/F7LY-OS-final-design.pdf`。
- 逐提交依据：`docs/report-src/commit-changelog-2026.md`。
- 当前事实依据：源码、最近提交、`AGENTS.md`、`agent_docs/` 和已完成的性能/网络计划。
- 所有“通过”“提升”“支持”结论必须附当前代码对应的运行日志、截图或评测结果，不能只引用中间提交标题。

旧报告中需要先纠正的关键表述：

1. 内核当前是 C++23 freestanding，明确禁用异常和 RTTI；不再沿用“完整支持 C++ 异常”的旧表述。
2. LoongArch 启动和硬件信息以 DTB、APIC/ExtIOI、virtio-pci 等当前实现为准，不能继续把 ACPI 写成唯一发现路径。
3. 根文件系统以 ext4 为主，FAT32 主要作为数据盘或回退挂载，不再写成对等的主根文件系统。
4. 用户态入口不仅有自动回归 initcode，还新增独立的 BusyBox ash 交互式 shell 模式。
5. 系统调用数量、Linux ABI 覆盖和评测范围需要以当前绑定表及四组合 scoreboard 为准。

## 2. 推荐目录

### 第一章 F7LY-OS 内核概述

#### 1.1 项目目标与 2026 年演进方向

- 面向教学、比赛、Linux ABI 和回归评测的双架构宏内核。
- 从“可启动、可运行基础测例”演进到“支持动态用户程序、复杂 Linux 语义和可重复性能评测”。
- 2026 年主线目标：双架构一致性、Linux ABI 正确性、长回归稳定性、I/O 与线程性能。

素材安排：

- 示例代码：Makefile 中 C++23、freestanding、`-fno-exceptions`、`-fno-rtti` 配置。
- 截图：RISC-V 与 LoongArch 启动完成画面并排。
- 图：2026 年整体架构图。

#### 1.2 当前系统能力总览

- 架构：RISC-V virt 与 LoongArch virt。
- 内存：伙伴系统、内核堆、slab、用户页表、VMA、mmap、共享内存。
- 进程：fork/clone/clone3/exec/wait、线程、信号、futex、POSIX timer。
- 存储：统一 virtio-blk、ext4 根文件系统、FAT32 数据盘、loop、VFS 和虚拟文件。
- 网络：ONPS/VirtIO Net 基础框架与真实 loopback TCP/UDP。
- 用户态：musl/glibc 动态程序、BusyBox、交互式 shell、LTP 与性能测试。
- 工程：四组合 scoreboard、日志解析和可复现实验。

#### 1.3 分层架构与模块职责

建议按以下六层重新画图，不直接复用旧图：

1. 架构启动、HAL、trap 与设备发现。
2. 内存、进程、线程与 IPC。
3. Linux ABI 与系统调用。
4. VFS、文件对象、ext4/FAT32 与块设备。
5. socket、loopback 与 ONPS/VirtIO Net。
6. 用户程序、shell、回归与评测工具。

#### 1.4 2025 与 2026 能力对照

使用表格列出：

| 领域 | 2025 文档状态 | 2026 当前状态 | 变化类型 |
| --- | --- | --- | --- |
| C++ 运行环境 | 旧文档宣称支持异常 | C++23 freestanding，禁用异常/RTTI | 纠正 |
| 内存发现 | 固定布局为主 | DTB + 动态物理内存区间 | 改进 |
| 用户地址空间 | 初步进程内存管理器 | 统一 VMA/mm 生命周期 | 改进 |
| 块 I/O | 架构独立驱动 | 统一队列与 priority-borrow | 新增/替换 |
| 网络 | BSD socket/协议栈框架 | 真实 loopback、批量消息、吞吐测试 | 改进 |
| 用户入口 | 自动回归 | 自动回归 + 交互式 shell | 新增 |
| 评测 | 分散日志 | 四组合 scoreboard 与流水线 | 新增 |

### 第二章 2026 年新增功能

本章每个功能统一使用“需求背景 -> 对外能力 -> 核心结构 -> 执行流程 -> 使用方法 -> 代码示例 -> 验证证据 -> 局限”八段式写法。

#### 2.1 双架构统一 virtio-blk 框架

##### 2.1.1 功能

- 将 RISC-V MMIO 和 LoongArch PCI 的传输差异封装在 transport/架构适配层。
- 通用层统一管理 request、descriptor、pending、inflight、completion 和 buffer 回写。
- ext4、bio 和普通文件不再直接依赖某一架构的磁盘队列实现。

##### 2.1.2 原理

- `VirtioBlkTransport` 负责设备交互。
- `VirtioBlkDevice` 负责块设备抽象。
- `VirtioBlkQueue` 负责请求排队、descriptor 分配、提交与完成回收。
- `IoRequest` 携带读写方向、块号、进程 PID/nice 和完成状态。

##### 2.1.3 使用与示例

- 构建并运行双架构 iozone。
- 示例代码选取 `VirtioBlkQueue::submit()` 到 `dispatch_pending_locked()` 的关键片段，不复制完整驱动。

##### 2.1.4 截图

- 双架构设备初始化日志。
- iozone 四组合结果或官方 judge 摘要。

#### 2.2 优先级调度与空闲带宽借用

##### 2.2.1 功能目标

- A、B 同时发起 I/O 时优先服务高优先级 A。
- A 未用满时允许 B 借用空闲带宽。
- A 停止后 B 可使用全部设备带宽。
- 同一优先级内按进程 flow 轮转，避免同级饥饿。

##### 2.2.2 原理

- nice 值映射为多个 service class。
- 每个 class 下维护 per-process flow 队列。
- 调度时优先选择最高非空 class；同 class 使用 round-robin。
- 实验模式限制 dispatch window，并对低优先级提交做节流，形成可观察的优先级差异。
- 说明 mClock 中间实现为何删除：实现复杂且实验结果不稳定，最终只保留一套权威调度器。

##### 2.2.3 使用与示例

- 运行 `priority_borrow_research()`。
- 示例代码：class 映射、enqueue/dequeue 和带宽借用判断。
- 给出三种负载阶段的预期输出。

##### 2.2.4 截图

- A/B 吞吐随时间变化曲线。
- 调度 trace 中 `high_wins`、`low_while_high_pending` 等统计。

#### 2.3 真实 loopback TCP/UDP

##### 2.3.1 功能

- TCP：bind/listen/connect/accept、backlog、双向 stream、close/shutdown 唤醒。
- UDP：bind、datagram queue、sendto/recvfrom、源地址回填。
- 支持阻塞/非阻塞、队列背压、信号中断、IPv6 loopback 兼容入口。
- 支持 `MSG_MORE`、sendmmsg/recvmmsg、常用 socket option 和 poll/epoll 就绪判断。

##### 2.3.2 原理

- 全局 loopback 端口表维护 listener 和 UDP binding。
- TCP connect 创建成对 socket_file，并通过接收队列和 sleep/wakeup 传递 payload。
- UDP 每次发送生成独立 datagram，保持消息边界。
- 说明 loopback 快路径与 ONPS/VirtIO Net 框架的边界；外网能力只有在补充当前运行证据后才写入正文。

##### 2.3.3 使用与示例

- TCP echo、UDP echo、iperf、netperf。
- 示例代码：TCP connect 配对或 UDP datagram 入队。

##### 2.3.4 截图

- TCP/UDP payload smoke。
- iperf 与 netperf 成功结果。
- socket LTP 小集合 PASS 摘要。

#### 2.4 双架构交互式 BusyBox ash

##### 2.4.1 功能

- `make shell r` 与 `make shell l` 使用独立 rootfs 镜像进入 BusyBox ash。
- 支持控制台输入、termios、当前目录、环境变量、文件访问和正常退出。
- 评测入口 `make run r/l` 保持自动回归，不与 shell 模式混用。

##### 2.4.2 原理

- Makefile 通过 `INITCODE_MODE` 选择 evaluation 或 shell initcode。
- UART/SBI -> console 行规程 -> device_file -> fd 0 -> BusyBox ash。
- shell 初始化后 `execve` BusyBox，并在退出后回到 initcode。

##### 2.4.3 使用与示例

```bash
make shell r
make shell l
```

演示命令规划：`pwd`、`ls /`、`cat /proc/version`、创建并执行 `.sh` 脚本、正常 `exit`。

##### 2.4.4 截图

- 双架构 shell 欢迎界面。
- rootfs 目录浏览与脚本执行。
- shell 退出后的结束标记。

#### 2.5 动态 ELF 与 shebang 执行链

##### 2.5.1 功能

- 支持 `PT_INTERP` 动态解释器路径读取与 musl/glibc loader 重写。
- 按 ELF `p_align` 装载，保留 LoongArch 16 KiB 对齐要求。
- 支持 `#! interpreter [optional-arg]` 参数重写。
- 支持 shell 中直接执行评测脚本。

##### 2.5.2 原理

- exec 先识别脚本或 ELF，再构建统一 argv。
- 动态 ELF 同时映射主程序和解释器，并填写 auxv。
- exec 成功后替换 mm、关闭 CLOEXEC fd、设置 PC/SP。

##### 2.5.3 使用、代码与截图

- 示例：一个最小 shebang 脚本和一个动态 BusyBox 命令。
- 代码：`parse_shebang_line()` 与 `PT_INTERP` 分支。
- 截图：脚本成功执行及动态 loader 路径日志。

#### 2.6 epoll 与事件驱动文件对象

##### 2.6.1 功能

- `epoll_create1`、`epoll_ctl`、`epoll_pwait`、`epoll_pwait2`。
- 普通文件、pipe、socket、virtual file 通过统一 `read_ready/write_ready` 接口参与就绪检查。

##### 2.6.2 原理

- `epoll_file` 保存关注 fd 与事件掩码。
- wait 路径扫描 ready 状态，并与 signal mask、timeout 配合。

##### 2.6.3 使用、代码与截图

- 示例：pipe + epoll 等待。
- 截图：epoll LTP 测例 PASS。

#### 2.7 capability、timex 与设备兼容层

分成四个短节：

1. `CapabilityManager`：capget/capset、bounding/ambient 能力查询与修改。
2. `TimexController`：adjtimex/clock_adjtime 的统一参数和状态处理。
3. `ConsoleTermios`：将 tty/termios 状态从巨型 syscall 文件中抽离。
4. `FileDescriptorAccess`、`SocketIoctlCompat`、`BlockDeviceIoctlState`：集中处理 fd 查找与常见 Linux 兼容视图。

每节只选一个核心结构和一个 LTP 证据，避免把系统调用列表写成流水账。

#### 2.8 四组合评测与可复现实验基础设施

##### 2.8.1 四组合模型

- RISC-V + musl。
- RISC-V + glibc。
- LoongArch + musl。
- LoongArch + glibc。

##### 2.8.2 工具

- `ltp_testcases[]` 四开关。
- `scoreboard/` 分组合状态。
- runner -> raw log -> parser -> ranker -> scoreboard。
- iozone/libcbench/priority-borrow 研究入口。

##### 2.8.3 截图

- scoreboard 汇总。
- LTP pipeline 输出目录。
- iozone 与 libcbench baseline 对比。

### 第三章 2026 年改动、删除与改进

本章强调“旧设计有什么问题、为什么改、改成什么、效果如何”，避免与第二章重复讲 API。

#### 3.1 从分散 VMA 修补到统一地址空间所有权

##### 3.1.1 改动前问题

- ELF、heap、mmap、共享内存、页表和退出回收跨模块管理。
- fork/clone/exec 对 mm 的复制、共享和替换边界不清晰。
- page fault 与 copy_in/copy_out 的懒分配路径不统一。

##### 3.1.2 改动后设计

- `ProcessMemoryManager` 统一管理页表、程序段、heap、VMA、mmap cursor 和引用计数。
- `CLONE_VM` 共享 mm，fork 深拷贝，exec 成功后原子替换。
- VMA 元数据、缺页分配、munmap/mremap、共享页和退出释放形成闭环。

##### 3.1.3 证据

- mmap/shm LTP。
- libcbench malloc/pthread。
- 地址空间图与一次缺页时序图。

#### 3.2 DTB 驱动的内存与启动布局

- 说明 1 月固定 128 MiB 适配与 5 月 DTB 动态内存发现之间的演进。
- 对比固定 `PHYSTOP` 与从 DTB 推导 RAM/initrd/保留区。
- 展示 PMM 如何切分内核、heap、共享内存和普通页区。
- 截图：两架构 DTB 与内存区间日志。

#### 3.3 LoongArch trap、TLB 与 LL/SC 稳定化

- ECODE 与 pending bit 分离，避免把同步异常误判为 timer 中断。
- timer pending 先清除，再在锁外执行复杂逻辑。
- 用户返回窗口最后恢复 sp/tp/t 寄存器，避免内核态使用用户栈。
- 动态 trapframe 映射和按页 TLB 失效。
- pthread/线程退出路径中的 LL/SC 问题、无效修复回退和最终约束。
- 截图：pthread/libcbench/lmbench 通过证据；不要使用旧调试 panic 作为成功图。

#### 3.4 进程、线程、信号与 futex 语义

- clone/clone3 flags、TGID/TID、TLS、parent/child tid。
- `CLONE_CHILD_CLEARTID` 退出清零并 futex wake。
- robust list、SIGCHLD、wait status 与进程组。
- `setpriority/getpriority` 如何同时服务 CPU 调度与 I/O 请求分类。
- 对比旧 PCB 大包大揽与当前共享 mm/files/sighand 的关系。

#### 3.5 VFS、ext4 与挂载语义

- 根文件系统策略：ext4 主盘、initrd 回退、FAT32 数据盘。
- 文件对象接口扩展：normal/device/pipe/socket/virtual/epoll。
- mount、umount、bind mount 与路径视图。
- virtual `/proc`、`/etc`、`/dev`。
- fcntl、ioctl、splice、xattr、fanotify、memfd 的改进。
- ext4 cache、批量读写、映射写回与性能优化。

截图/示例：

- shell 中 `mount`、bind mount 前后目录视图。
- `splice` 或 pipe 数据搬运。
- `/proc` 与 `/etc` 虚拟文件读取。

#### 3.6 系统调用组织与 Linux ABI 语义

- 绑定数量从 224 增至 243，但正文重点写“语义质量”而非只写数量。
- syscall 表默认填充 ENOSYS，再显式绑定。
- 参数获取、用户拷贝、负 errno 和 ABI 结构统一。
- 从单一巨型文件向 `syscall_abi`、`sysio`、`sysproc` 和领域管理类拆分。
- 新增/补齐 syscall 按进程、文件、网络、时间、事件、权限分组列表放附录。

#### 3.7 性能与长回归优化

- 块队列并发与 descriptor 回收。
- ext4/bcache/normal_file 批量路径。
- exec 文件读取与缓存，减少每个测例启动开销。
- SIGCHLD 与进程清理去重。
- 测例排序与单项/长回归策略。

证据：

- iozone 四组合总分 89.703，高于计划 baseline floor 80。
- libcbench 四组合总分 129.973，高于计划 baseline floor 108。
- `629b213` 前后的同一批测例运行时间对比需要在正式写作阶段补测，不能只写“明显提升”。

#### 3.8 构建、运行与仓库结构改进

- evaluation/shell 两种 initcode 模式。
- 双架构构建、QEMU 内存参数、snapshot 防污染。
- GDB、挂载、镜像、日志和 LTP 工具迁入规范目录。
- 所有复现命令放在附录，正文只保留最常用命令。

#### 3.9 删除与替换

明确列出：

1. 删除 RISC-V 旧 `virtio2.hh`，替换为统一 virtio-blk。
2. 删除 mClock 调度器，替换为 priority-borrow。
3. 删除递归符号链接解析，替换为受限迭代。
4. 删除根目录旧 mount/GDB/日志入口，迁移到标准目录。
5. 删除误提交运行日志和旧 LTP raw 文件。
6. 删除重复 LTP 执行项和分散隐藏入口，统一使用四组合测试表。

每项写明删除原因、替代方案和迁移后的效果。

### 第四章 验证与效果

#### 4.1 验证环境

- 工具链、QEMU 版本、内存、镜像来源、commit hash。
- 强调 LoongArch 原子语义对 QEMU 版本敏感，正式结果需固定环境。

#### 4.2 功能验证矩阵

按“功能 -> RISC-V -> LoongArch -> musl -> glibc -> 证据路径”制作表格。

#### 4.3 Linux ABI 与 LTP

- 展示四组合开关、scoreboard 和代表性测试族。
- 不在正文堆叠两千多个测例；完整列表放附录或引用 scoreboard。

#### 4.4 性能结果

- iozone。
- libcbench。
- iperf/netperf。
- priority-borrow 三阶段实验。
- 测例启动耗时优化。

#### 4.5 局限与后续工作

- old/new file 抽象仍有并存路径，需要继续收口。
- LoongArch pthread、LL/SC 和 TLB 仍应持续回归。
- 外部网络链路只有具备当前日志证据后才宣称完成。
- fanotify、io_uring、bpf 等接口中可能存在兼容或占位实现，附录必须区分“完整”“部分”“仅错误码兼容”。

### 第五章 总结

- 概括 2026 年从“功能堆叠”到“Linux 语义、双架构一致性和可重复验证”的转变。
- 总结统一所有权和统一队列等设计原则。
- 不重复罗列 syscall 名称。

### 附录

1. 新增/改动/删除功能总表。
2. 2026 年 syscall 变化表及支持级别。
3. 四组合 scoreboard 链接。
4. 构建、shell、QEMU、GDB 和单测复现命令。
5. 图表、截图与日志来源索引。
6. 逐提交变更整理文档链接。

## 3. 截图与示例代码清单

### 3.1 必须补充的截图

| 编号 | 截图内容 | 用途 | 建议来源 |
| --- | --- | --- | --- |
| S1 | RISC-V 启动完成、DTB、内存和 rootfs | Overview/启动改进 | `make run r` 日志 |
| S2 | LoongArch 启动完成、DTB、内存和 rootfs | 双架构对照 | `make run l` 日志 |
| S3 | RISC-V BusyBox ash 执行 `pwd/ls/script/exit` | 新增 shell | `make shell r` |
| S4 | LoongArch BusyBox ash 同类操作 | shell 双架构性 | `make shell l` |
| S5 | TCP/UDP payload smoke | 真实 loopback | 单项网络自测 |
| S6 | iperf/netperf 成功结果 | 网络吞吐与并发 | 对应回归日志 |
| S7 | iozone 官方 judge 汇总 | I/O 性能 | 四组合结果 |
| S8 | libcbench 官方 judge 汇总 | 线程/内存性能 | 四组合结果 |
| S9 | priority-borrow A/B 吞吐曲线 | 调度策略效果 | 研究入口输出转图 |
| S10 | mount/bind mount 前后目录 | VFS 改进 | shell 或 LTP |
| S11 | epoll/fanotify/splice 代表测例 PASS | 新 ABI | 小集合日志 |
| S12 | scoreboard 顶层汇总 | 评测工程 | `scoreboard/README.md` |

### 3.2 示例代码选取原则

- 每段控制在 15 至 35 行，只展示关键不变量和主流程。
- 优先引用当前源码，不从旧 PDF 复制已经过时的代码。
- 每个功能最多放两段代码：一段核心结构，一段关键执行流程。
- 代码下方必须解释输入、锁、所有权、错误路径和跨架构差异。

建议代码位置：

| 主题 | 建议源码 |
| --- | --- |
| VMA/mm | `kernel/proc/process_memory_manager.hh/.cc`、`kernel/proc/context.hh` |
| exec/shebang | `kernel/proc/proc_manager.cc` |
| priority-borrow | `kernel/fs/drivers/virtio_priority_borrow_scheduler.*` |
| virtio queue | `kernel/fs/drivers/virtio_blk_queue.*` |
| loopback socket | `kernel/fs/vfs/file/socket_file.cc` |
| epoll | `kernel/fs/vfs/file/epoll_file.hh`、`kernel/sys/syscall_handler.cc` |
| capability | `kernel/proc/capability.*` |
| timex | `kernel/tm/timex_controller.*` |
| mount/bind | `kernel/fs/vfs/vfs_utils.cc` |
| LoongArch trap | `kernel/trap/loongarch/trap.cc`、`uservec.S` |
| 测试入口 | `user/user_lib/user_test.cc` |

## 4. 需要手工绘制的架构图

建议统一使用 draw.io，源文件后续放入 `docs/assets/diagrams/`，导出 PNG/SVG 放入 `docs/assets/`。当前提纲阶段不创建空图片。

### 4.1 必画

1. **2026 年内核整体分层图**
   展示用户态、Linux ABI、进程/内存、VFS/网络、设备/架构层，以及 RISC-V/LoongArch 分叉。

2. **双架构启动与 DTB 驱动初始化图**
   展示 entry -> DTB -> PMM/VMM/HMM -> virtio -> VFS -> user init -> scheduler，并标注架构差异。

3. **进程地址空间与 VMA 生命周期图**
   展示 ELF、heap、mmap、共享内存、stack、trapframe，以及 mmap fault/fork/clone/exec/exit 对 mm 的操作。

4. **统一 virtio-blk 架构图**
   展示 ext4/bio -> device -> queue/scheduler -> transport -> RISC-V MMIO 或 LoongArch PCI。

5. **priority-borrow 队列组织图**
   展示 service class、per-process flow、同级轮转、高优先级优先和低优先级借用空闲带宽。

6. **VFS 与 mount/bind 路径解析图**
   展示进程 cwd/root、挂载视图、虚拟文件树、ext4/FAT32、file 派生对象和 fd 表。

7. **loopback TCP/UDP 数据路径图**
   展示 syscall -> socket_file -> 端口表/listener/datagram queue -> peer，以及 poll/epoll、sleep/wakeup。

8. **进程线程、信号与 futex 关系图**
   展示 PID/TID/TGID、mm/files/sighand 共享、clone flags、robust futex、SIGCHLD 和 wait。

9. **系统调用分层与分发图**
   展示架构 trap -> 参数提取 -> syscall 表 -> sysio/sysproc/领域管理器 -> VFS/proc/mem。

10. **四组合评测流水线图**
    展示 initcode/test table -> QEMU log -> parser/ranker -> scoreboard -> 文档证据。

### 4.2 可选

1. LoongArch usertrap/usertrapret、TLB 与 trapframe 时序图。
2. execve 动态 ELF + PT_INTERP + shebang 参数重写图。
3. epoll 关注集与多类 file 就绪状态图。
4. 2025 到 2026 的架构演进对照图。

## 5. 后续正文写作顺序

1. 先固定截图与日志证据，确认哪些能力可以使用“已支持/已通过”。
2. 重画整体架构、VMA、块 I/O、VFS、网络五张核心图。
3. 完成第一章 overview 和 2025/2026 对照表。
4. 按第二章功能逐节补原理、用法、代码和截图。
5. 按第三章解释改动动机、删除项和改进效果。
6. 最后生成 syscall 附录、验证矩阵和引用索引。


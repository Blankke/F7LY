# F7LY-OS 2026 年文档增量：逐提交变更整理

## 1. 整理口径

- 去年成品基线：`docs/archive/design/F7LY-OS-final-design.pdf`，生成时间为 2025 年 8 月。
- 第一阶段：`9975964036dcdd494ca83e35524b26dbf0656fef^..e47ee9bc1e5e1ca19a6cafb135c40aedfa61f1fd`，共 13 个提交。
- 第二阶段：`e47ee9bc1e5e1ca19a6cafb135c40aedfa61f1fd^..HEAD`，截至 2026 年 6 月 14 日共 96 个提交。
- `e47ee9b` 同时是第一阶段终点和第二阶段起点，因此在两张表中都会出现。
- 每条说明综合提交标题、提交正文、变更文件和当前源码判断。中间调试、回退和重复合并会如实记录，但不会直接作为最终报告中的独立功能。
- 第二阶段相对起点累计改动 388 个文件；其中内核 146 个文件约新增 44826 行、删除 17160 行，系统调用绑定由 224 个增至 243 个。

## 2. 第一阶段：2026 年 1 月至 5 月启动标记

| 提交 | 日期 | 类型/模块 | 变更整理 |
| --- | --- | --- | --- |
| `9975964` | 2026-01-12 | 构建 | 调整默认构建与运行流程，补充 RISC-V GCC 11 工具链准备，并将 initrd 从备份镜像恢复后交给 QEMU。 |
| `7fffd7a` | 2026-01-12 | 构建/日志 | 修正 RISC-V 内核产物名为 `kernel-qemu`，QEMU 内存降为 128 MiB，切换到 `initrd.img`，临时打开内核打印。 |
| `5df2a9c` | 2026-01-12 | 内存管理 | 重构伙伴系统、物理内存和堆内存初始化，使双架构能在 128 MiB 内存配置下工作。 |
| `66193ac` | 2026-01-12 | VFS | 修正 `fstat` 路径中的文件状态返回逻辑。 |
| `60f2338` | 2026-01-12 | 文档 | 向 2025 年 Typst 报告补充内核详细设计内容；属于旧报告维护，不是内核功能增量。 |
| `e189b7a` | 2026-01-13 | LoongArch 启动 | 尝试接入 LoongArch 启动参数中的 DTB 地址，调整链接地址、128 MiB 物理内存布局、initrd 和测试入口。该实现随后继续修正。 |
| `859720c` | 2026-01-13 | 挂载/块设备 | 修正 LoongArch DTB 参数寄存器判断，并让 ext4 块设备读写使用实际设备号，根设备由硬编码 1 改为 0。 |
| `ed38158` | 2026-01-14 | DTB/initrd | 增加 DTB 有效性检查、内存扫描和 `/chosen` initrd 信息解析，启动时据此初始化文件系统。后续 5 月实现覆盖了部分探测逻辑。 |
| `892b2d9` | 2026-01-14 | RISC-V 启动 | 补回 RISC-V DTB 初始化，修复上一轮双架构改动造成的 RISC-V 启动回退。 |
| `f789f0d` | 2026-01-14 | 基础库/日志 | 重构 printer，统一控制台输出与缓冲区写入，精简并优化 `printf` 格式化实现。 |
| `decd2d7` | 2026-01-14 | 基础库 | 整理 common/function/klib/template_algorithm 等基础库，删除重复实现并统一接口。 |
| `6a11b9b` | 2026-01-14 | 进程/调度 | 整理 PCB 初始化和 CPU mask，补充用户态时间字段，修正调度器函数命名与状态过滤。 |
| `e47ee9b` | 2026-05-19 | 阶段标记 | 更新 README，标记 2026 年 5 月集中开发开始；无实质内核功能。 |

第一阶段最终可进入年度报告的内容较少，主要是：128 MiB 环境适配、DTB/initrd 启动探测、打印基础设施重构。其余为构建修正、文档维护或很快被第二阶段覆盖的中间实现。

## 3. 第二阶段：2026 年 5 月至 6 月集中开发

| 提交 | 日期 | 类型/模块 | 变更整理 |
| --- | --- | --- | --- |
| `e47ee9b` | 2026-05-19 | 阶段标记 | 更新 README，开启 2026 年集中开发。 |
| `5c0c055` | 2026-05-19 | 启动/文件系统/日志 | 调整日志系统、FAT32 与 VFS 初始化、启动挂载和回归入口，为新评测镜像与双架构运行做准备。 |
| `699e042` | 2026-05-20 | VMA/进程内存 | 大规模修复 VMA、mmap、页表、共享内存、文件映射与进程地址空间生命周期，并初步加入 epoll 文件对象。 |
| `9414b43` | 2026-05-20 | 文件对象/系统调用 | 继续修正 file 抽象、堆管理、普通文件和 syscall 语义，收敛上一提交的回归问题。 |
| `5867d97` | 2026-05-20 | exec/VFS/缺页 | 补充路径与装载逻辑、进程创建和 RISC-V 缺页处理，使更多 LTP 程序能够进入运行。 |
| `26c977b` | 2026-05-20 | LTP/内存 | 完善 VFS、进程内存与 syscall 细节，打通首批 LTP 运行链路，并清理临时 RISC-V trap 补丁。 |
| `ce23788` | 2026-05-20 | 清理 | 删除 VFS 与 initcode 中的临时调试代码。 |
| `5582323` | 2026-05-20 | LoongArch | 调整 LoongArch 链接布局、trap、用户链接脚本和进程装载，修复一部分用户程序问题。 |
| `1324495` | 2026-05-21 | LoongArch/回归 | 继续修正 LoongArch trap 和双架构测试入口，属于阶段性稳定化。 |
| `1b894aa` | 2026-05-21 | Linux 语义/时间/内存 | 集中完善 VFS、pipe、VMA、POSIX timer、信号和大量 syscall 错误语义；当时仍保留一处卡死。 |
| `ac2b88d` | 2026-05-21 | DTB/动态内存 | 为 LoongArch 正式接入 DTB 内存信息，并重构伙伴系统、物理内存、堆和 VMM，减少固定内存边界假设。 |
| `0cbf984` | 2026-05-21 | 进程退出/并发 | 在 LoongArch 进程退出清理期间增加关中断保护，并修正 futex、信号、timer 与 trap 的并发清理。 |
| `de69214` | 2026-05-21 | LTP/VFS | 扩展 LTP 清单，修正文件系统与 syscall 语义，推进批量测例运行。 |
| `42eaa49` | 2026-05-21 | personality | 实现并绑定 `personality`，保存进程 personality 状态并接入回归。 |
| `c85b2c5` | 2026-05-21 | 仓库清理 | 删除误提交的超大 QEMU 输出，并将运行日志加入忽略规则。 |
| `87b9eec` | 2026-05-21 | 进程优先级 | 完整实现 `setpriority/getpriority`，维护 PCB nice 值，并补充 `/proc` 虚拟信息与 LTP 回归。 |
| `47829cc` | 2026-05-22 | LoongArch trap/VFS | 将符号链接解析由递归改为迭代，修复深链路内核栈溢出；同时修正 LoongArch 中断判定、timer pending 清理和用户返回窗口寄存器恢复。 |
| `9e576c4` | 2026-05-22 | 合并/清理 | 合并 regression 分支，仅补充日志忽略规则，无独立功能。 |
| `ff72e8c` | 2026-05-22 | 合并 | 合并 LoongArch trap 分支与优先级分支；提交标题重复 `setpriority/getpriority`，合并提交本身无额外树差异。 |
| `91968d6` | 2026-05-22 | 块 I/O 研究 | 增加模拟 I/O 场景、请求元数据和用户态研究入口，开始探索按进程区分的 I/O 调度。 |
| `f674306` | 2026-05-22 | 注释/研究工具 | 为 RISC-V virtio 磁盘和 iozone 研究代码补充 Doxygen 注释并整理结构。 |
| `3c0be3c` | 2026-05-23 | LoongArch pthread/trap | 深入修复 LoongArch TLB、LL/SC、trap 返回、futex、signal、timer 和内存分配，并加入原子指令探针。 |
| `3bfd587` | 2026-05-24 | 块 I/O/IOzone | 从 RISC-V 磁盘驱动中拆出独立 I/O 调度逻辑，完善 iozone 入口，同时整理 pipe 与 ext4 块设备路径。 |
| `55a26f1` | 2026-05-24 | LoongArch 回退/诊断 | 回退部分无效 pthread 修复，保留 LL/SC 探针与补丁工具，将问题收敛到可复现状态。 |
| `bb56d82` | 2026-05-25 | 仓库结构 | 将旧报告、开发笔记、GDB、挂载脚本和 LTP 工具归档到规范目录，重写 README/AGENTS 并清理根目录。 |
| `54c4ae2` | 2026-05-25 | 协作规范 | 更新 AGENTS 协作规则，无内核功能变化。 |
| `6bdca8c` | 2026-05-25 | 统一 virtio-blk/性能 | 建立跨架构 `VirtioBlkDevice`、`VirtioBlkQueue`、transport、request 和调度器；重写双架构块驱动，并优化 ext4 cache、文件 I/O、内存与共享内存，使 iozone 可完整运行。 |
| `b3f0965` | 2026-05-25 | 文件锁/key/socket | 修复 `flock` 系列、`add_key` 系列和 `accept` 语义，完善 file/socket 与 syscall 路径。 |
| `177123b` | 2026-05-25 | 合并 | 将 iozone/块 I/O 分支合并到 regression，无额外独立功能。 |
| `d234b3c` | 2026-05-25 | 回归入口 | 整理 initcode、测试选择、用户依赖和 LL/SC 探针，统一双架构回归入口。 |
| `c40a7aa` | 2026-05-25 | 管道 | 修复 pipe sleep/wakeup 小概率问题。 |
| `a74491f` | 2026-05-25 | LoongArch pthread | 继续围绕 CPU 状态、VMM、signal、trap 和 LL/SC 修复 pthread 卡死。 |
| `54064ca` | 2026-05-25 | 工具脚本 | 整理双架构镜像挂载脚本。 |
| `c858ac1` | 2026-05-25 | 评测基础设施 | 增加四组合 scoreboard、生成器和架构/调试/协作文档。 |
| `979ac9b` | 2026-05-25 | 信号/futex/libctest | 重构信号、futex、线程退出和定时器语义，使 libctest 通过，并记录 libcbench/heap 的后续问题。 |
| `57c5875` | 2026-05-25 | 合并 | 同步 regression 远端分支，无可单独归纳的最终功能。 |
| `9cac440` | 2026-05-25 | Scoreboard | 修正 scoreboard 数据格式、状态与生成说明。 |
| `7cd31f9` | 2026-05-25 | 合并 | 再次同步 regression 分支，无独立功能。 |
| `897a963` | 2026-05-26 | 开发规范 | 更新构建与调试规范，无内核功能变化。 |
| `783e881` | 2026-05-26 | Loopback 网络 | 建立可真实传递 payload 的本机 TCP/UDP socket 路径，补充 socket ABI、自测和用户态封装。 |
| `7bd53d6` | 2026-05-26 | 双架构回归稳定化 | 集中修复文件、内存、进程、定时器、信号、共享内存和 trap，使双架构长回归可以跑完，并同步 scoreboard。 |
| `ff5636b` | 2026-05-26 | 计划文档 | 增加网络修复与外网访问计划，后续完成后改名为 `done_*`。 |
| `0dbad02` | 2026-05-26 | 仓库清理 | 停止跟踪根目录运行日志 `riscv.txt`。 |
| `6f252e6` | 2026-05-27 | 时间/评测状态 | 修正时间管理和部分 syscall，更新双架构 LTP 状态与回归入口。 |
| `b66f29a` | 2026-05-27 | 交互式 Shell/控制台 | 新增 BusyBox ash shell 入口，统一设备文件 read 契约，接通 UART 输入、LoongArch UART0 中断与控制台行规程。 |
| `89db95b` | 2026-05-27 | 用户态兼容 | 增加 shell/initcode 需要的兼容标志与用户态辅助对象。 |
| `cfdaf35` | 2026-05-27 | Shell/VFS | 修正 shell 根目录、磁盘路径、`pwd` 和环境初始化，使 shell 能浏览根文件系统。 |
| `7c6cbe6` | 2026-05-27 | Socket 批量发送 | 实现 `MSG_MORE` 延迟发送语义，补齐 `sendmmsg` 与批量 socket 测试入口。 |
| `a2dc3e0` | 2026-05-27 | exec/shebang | 修复 shell 执行脚本，完善虚拟文件、路径解析与 `execve` 的 shebang 解释器重写。 |
| `700cca3` | 2026-05-28 | LTP 计划 | 更新 LTP 提升目标和双架构测试入口。 |
| `eb73966` | 2026-05-28 | 网络吞吐 | 完善 loopback TCP/UDP 的阻塞、非阻塞、队列背压、accept 中断、连接竞态、IPv6 兼容和常用 socket option；RISC-V musl/glibc 的 iperf/netperf 通过。 |
| `6d18d19` | 2026-05-28 | 合并 | 将网络修复合并到主线，解决 syscall 与回归入口冲突，并保持双架构构建通过。 |
| `ef0e617` | 2026-05-28 | 回归入口 | 调整双架构 initcode 的测试组合。 |
| `9bccd94` | 2026-05-28 | 网络稳定性 | 修复 netperf 连续运行时 server 启动竞态，并让阻塞 socket 接收能够被信号中断。 |
| `de2f2c9` | 2026-05-29 | epoll/clock/fcntl | 扩展 clock、epoll、fcntl 测例，完善 epoll 文件对象、就绪判断、signal 与相关 syscall。 |
| `29d312c` | 2026-05-29 | 回归入口 | 增加一批双架构测试项，无独立内核机制。 |
| `c9ac7c4` | 2026-05-29 | 共享内存测例 | 加入 shm 测例与功能标注，当时尚未完成实现修复。 |
| `2dcfbe0` | 2026-05-29 | fcntl/信号 | 修复 `fcntl14/fcntl14_64` 涉及的所有者、异步通知或信号语义。 |
| `48fbc8a` | 2026-05-30 | 计划归档 | 将已完成的网络、外网和 LoongArch 任务文档改名归档。 |
| `62eff70` | 2026-05-30 | 网络测例 | 继续完善 socket 定义、socket_file 与网络 syscall，并扩展回归。 |
| `c6d6069` | 2026-05-30 | 性能计划 | 增加 iozone 与 libcbench 优化计划。 |
| `a1a8ea8` | 2026-05-30 | mmap basic | 修复 basic `test_mmap` 对应的进程地址空间行为。 |
| `3614f07` | 2026-05-30 | 评测任务 | 更新 LTP 任务板与测试入口。 |
| `b419945` | 2026-05-30 | I/O 调度器替换 | 删除 mClock 调度器，替换为多优先级、按进程 flow 轮转的 priority-borrow 调度器，并调整实验入口。 |
| `fb81eed` | 2026-05-31 | 性能调试 | 针对 libcbench/iozone 未达基线补充内存、文件、trap 和回归调试改动；该提交是中间态。 |
| `4099546` | 2026-05-31 | I/O 实验 | 增加手写长时 I/O 压测，完善优先级借用调度、进程 nice 传递和观测统计，用于验证高优先级压制与空闲带宽借用。 |
| `dee723b` | 2026-05-31 | shm/mmap/libctest | 修复 System V 共享内存、mmap、VMA 和虚拟文件语义，整理回归并完成 libctest。 |
| `dde2ae4` | 2026-06-01 | iozone/libcbench 性能 | 优化块队列、进程创建、地址空间与 syscall 快路径；四组合 iozone 与 libcbench 达到并超过计划中的 baseline。 |
| `b6727c1` | 2026-06-01 | 仓库清理 | 更新忽略规则。 |
| `afe93e2` | 2026-06-01 | 合并 | 合并 regression 分支，无独立功能。 |
| `3b56d89` | 2026-06-01 | 合并 | 合并 iozone 分支，无独立功能。 |
| `e1133bf` | 2026-06-01 | 回归入口 | 调整双架构测试入口和执行顺序。 |
| `f8ea061` | 2026-06-01 | memfd/并发 | 修复 `memfd_create` 路径在关中断状态下触发 panic 的问题，并完善 normal_file 与 mm 引用。 |
| `4fe2380` | 2026-06-01 | clone3 | 修复 `clone302` 对 `clone3` 参数和错误码的要求。 |
| `b8e52a7` | 2026-06-02 | LTP TFAIL 清理 | 修复 ext4、普通文件、VFS、POSIX timer、进程和 syscall 中当前已暴露的 TFAIL。 |
| `3890255` | 2026-06-03 | LoongArch/lmbench | 大规模修正 LoongArch 页表、VMM、物理内存、pipe、调度、进程和文件路径，解决 lmbench 运行问题。 |
| `bfa0e73` | 2026-06-03 | pipe/fcntl | 修复 `fcntl19` 中 pipe 小数据写入未唤醒读端的问题。 |
| `5f21ad6` | 2026-06-03 | SIGCHLD/性能 | 移除重复的子进程信号 bookkeeping，规范 SIGCHLD 逻辑并改善 libctest 运行速度。 |
| `90312ba` | 2026-06-03 | I/O 性能/回归顺序 | 增强普通文件和 VMM 快路径，调整进程与 syscall 行为，并将 iozone 放到更合适的评测顺序。 |
| `6356a47` | 2026-06-03 | LTP 工具链 | 建立四组合 LTP runner、parser、ranker 和输出目录，实现可重复的批量分析流程。 |
| `46780a9` | 2026-06-03 | SIGCHLD | 继续修正父子等待、退出通知和 SIGCHLD 发送条件。 |
| `decd5ce` | 2026-06-04 | 合并 | 合并主线 LTP 工具与结果，无独立内核功能。 |
| `4f5ac77` | 2026-06-04 | fanotify/ioctl | 增加 fanotify 测例与 syscall，完善虚拟文件、设备 ioctl、CPU 信息及相关错误语义。 |
| `1b6de72` | 2026-06-05 | splice/VFS | 修复 `splice07`，重构 VFS ops、normal/pipe/virtual file 的数据搬运和路径语义。 |
| `6102445` | 2026-06-05 | 信号/内存/syscall | 临时集中修正 file、VMM、进程 mm、signal 与 syscall 的边界行为，后续由重构提交吸收。 |
| `8f9457c` | 2026-06-06 | 回归清理 | 删除重复运行的 clock/clone 测例，重新打开 abort 至 writev 区间用于验证。 |
| `bd6da5f` | 2026-06-06 | Shell/rootfs | 让交互式 shell 默认使用独立 ext4 rootfs 镜像，增加双架构 rootfs 挂载脚本并调整 shell 初始化。 |
| `5319d08` | 2026-06-06 | syscall 架构/权限/文件 | 将部分 syscall 辅助逻辑拆到 `sysio/sysproc/syscall_abi`，新增 capability 管理、timex 控制、fd 访问器、termios 与块设备 ioctl 状态，并完善 renameat、xattr、loop、网络兼容等语义。 |
| `5727aaa` | 2026-06-06 | RISC-V 控制台 | 将 RISC-V 控制台输入统一切到 SBI 路径，并同步 trap 与块队列细节。 |
| `b5c4000` | 2026-06-07 | 回归入口 | 补齐少量遗漏测例。 |
| `f28f2ed` | 2026-06-07 | fs_bind 测例 | 增加新一批 LTP，标记待修项目，并暴露 bind mount 语义缺口。 |
| `dd6b79a` | 2026-06-08 | 评测状态 | 标注四组合中的 glibc LTP 运行结果，无内核功能变化。 |
| `31eea18` | 2026-06-10 | errno/进程 | 修正进程管理和 syscall 的 Linux errno 返回语义。 |
| `3803a69` | 2026-06-14 | mount/bind/exec | 实现更完整的 mount namespace 视图、bind mount、挂载路径解析与虚拟文件同步；同时完善 ELF 对齐、exec、进程状态和相关 LTP。 |
| `629b213` | 2026-06-14 | 回归性能 | 优化 exec/进程装载中的文件读取与缓存路径，显著缩短大量测例的启动时间。 |
| `10a501a` | 2026-06-14 | 文档任务 | 新增本次 2026 年文档提纲任务说明。 |

## 4. 重复改动归并

下列主题在多个提交中反复出现，正式报告应合并叙述，不按提交时间逐条展开：

| 归并主题 | 主要提交链 | 最终应描述的稳定结果 |
| --- | --- | --- |
| VMA 与进程地址空间 | `699e042`、`9414b43`、`5867d97`、`1b894aa`、`7bd53d6`、`a1a8ea8`、`dee723b`、`3890255` | `ProcessMemoryManager` 成为地址空间权威所有者，统一管理 ELF、heap、VMA、懒分配、共享映射、fork/clone/exec 和退出回收。 |
| LoongArch trap/线程 | `5582323`、`1324495`、`47829cc`、`3c0be3c`、`55a26f1`、`a74491f`、`3890255` | 修正 ECODE/中断判定、TLB、trapframe 映射、用户返回窗口和 LL/SC 场景，形成可运行 pthread、libcbench、lmbench 的稳定路径。 |
| 信号、futex 与父子生命周期 | `0cbf984`、`979ac9b`、`7bd53d6`、`de2f2c9`、`5f21ad6`、`46780a9` | 统一线程退出、robust futex、SIGCHLD、wait status、阻塞唤醒与资源清理语义。 |
| Socket 与网络回归 | `783e881`、`7c6cbe6`、`eb73966`、`9bccd94`、`62eff70` | 建立真实 loopback TCP/UDP 状态机，支持阻塞/非阻塞、批量消息、就绪通知和常用 socket option，并通过 iperf/netperf。 |
| 块 I/O 与性能 | `91968d6`、`3bfd587`、`6bdca8c`、`b419945`、`4099546`、`dde2ae4`、`90312ba` | 形成双架构统一 virtio-blk 队列、priority-borrow 调度器、ext4/cache 优化和可复现实验，iozone 达到 baseline。 |
| LTP 与评测基础设施 | `de69214`、`c858ac1`、`7bd53d6`、`6356a47`、`8f9457c`、`dd6b79a` | 建立四组合测试开关、scoreboard、日志解析和排名流水线，评测状态可追踪、可复现。 |
| 交互式 shell 与 exec | `b66f29a`、`cfdaf35`、`a2dc3e0`、`bd6da5f`、`3803a69` | 提供双架构 BusyBox ash 入口，支持 UART 输入、rootfs、动态 ELF、shebang 和挂载视图。 |
| 系统调用与 Linux ABI | `42eaa49`、`87b9eec`、`de2f2c9`、`4f5ac77`、`1b6de72`、`5319d08`、`31eea18`、`3803a69` | 系统调用绑定增加并按领域拆出辅助模块，补齐 priority、epoll、capability、fanotify、splice、mount 等语义与 errno。 |

## 5. 今年相对去年文档的最终增量

### 5.1 新增功能

1. 双架构统一 virtio-blk 队列与优先级带宽借用 I/O 调度。
2. 可真实传递数据的 loopback TCP/UDP，以及 iperf/netperf 所需的并发与 socket 语义。
3. 双架构交互式 BusyBox ash shell、rootfs 启动模式与 shebang 脚本执行。
4. epoll 文件对象及事件等待路径。
5. capability 管理、timex 控制、console termios、块设备 ioctl 状态和统一 fd 访问器。
6. 四组合 scoreboard、LTP runner/parser/ranker 与研究型性能实验入口。
7. 新增或正式接入的 Linux ABI，包括 `personality`、`setpriority/getpriority`、`sendmmsg/recvmmsg`、`execveat`、`fanotify`、`epoll_pwait2` 等。

### 5.2 重点改进

1. VMA、mmap、共享内存和进程地址空间生命周期从分散修补改为统一管理。
2. LoongArch 的 DTB、动态内存边界、trap/TLB/LL-SC 与 pthread 支撑得到系统性修复。
3. 信号、futex、clone/clone3、线程退出、SIGCHLD 和 wait 语义更接近 Linux。
4. VFS/ext4 的挂载、bind mount、虚拟文件、splice、fcntl、ioctl、xattr 和文件 I/O 语义得到扩展。
5. exec 支持动态链接器路径处理、ELF 对齐、shebang 重写和更快的装载缓存。
6. 构建系统区分评测与 shell 模式，镜像路径、GDB、挂载脚本和日志目录完成规范化。
7. iozone、libcbench、libctest、lmbench 和 LTP 的正确性及性能显著提升。

### 5.3 删除与替换

1. 删除独立的 RISC-V `virtio2.hh` 旧块设备接口，改用跨架构统一 virtio-blk 抽象。
2. 删除试验性的 mClock 调度器，统一替换为 priority-borrow 调度器。
3. 删除根目录旧 GDB、mount、日志与统计脚本入口，迁移到 `debug/`、`scripts/` 和 `tools/`。
4. 删除误提交的运行日志和旧 `ltp_judge/ltp_raw.txt`，改用受控日志目录和四组合流水线。
5. 删除递归符号链接解析等高风险路径，改为有深度上限的迭代实现。


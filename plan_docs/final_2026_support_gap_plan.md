# 2026 决赛内核能力缺口计划：CAgent / BuildStorm

状态：已完成静态摸底、8G/SMP 基础、final-2026 小门槛代码收尾、ext4/bcache 首轮并发修复和 schedbench 工具接入；官方双架构长压、CAgent/BuildStorm 完整回归和性能验收仍待完成

日期：2026-07-26

## 1. 范围和结论

本计划只评估“内核是否具备支撑两道题的能力”，不把 rootfs 制备、测试 runner、串口标记、judge 和源码注入算作内核缺口。它们只是后续验证这些内核能力的前提。

结论如下：

- CAgent：F7LY 的基础内核能力已经覆盖较多。动态 ELF/glibc、进程创建与 exec、基础时间、文件创建/读写/目录遍历、socket 和 TCP/UDP 路径都存在，暂时看不到必须重做内核架构的缺口。主要差距是若干 Linux 语义不完整或返回固定值，需要针对 10 个 CAgent 测试逐项验证。
- BuildStorm：动态 8G PMM、SMP HAL、通用 TLB shootdown 和实际 CPU 并行能力已经通过双架构 QEMU 短验收；尚缺 RISC-V selfhost rootfs 下的 Cargo/ext4 长压力证据。多线程/futex/信号、mmap/缺页/页回收、ext4/bcache 并发元数据与写回仍需由该长压力继续覆盖。
- 现有 syscall 绑定数量不能作为完成度指标。F7LY 的很多关键 syscall 已有函数实现，但仍带 TODO、固定返回值、错误路径 panic 或只覆盖部分 Linux 语义；应以 Rust 工具链和 CAgent 的真实 syscall 轨迹验收。
- [x] 2026-07-26：官方 `final2.1` 双架构镜像、8 核/8 GiB QEMU 参数和两道 glibc runner 已接入，代码完成待验收。首次实测已暴露 RV ext4 bcache 并发损坏，以及 LA 动态库搜索和 `/work/tgoskits` 访问失败，尚未达到赛题通过门槛。
- [x] 2026-07-26 收尾：移除内核中面向 Cargo/BuildStorm 的临时串口跟踪；LoongArch initcode 恢复为 CAgent + BuildStorm 双题入口；BuildStorm runner 保留 `/bin/sh /glibc/buildstorm_testcode.sh` 调用但去掉 `sh -x` 跟踪，避免污染官方日志。
- [ ] 2026-07-26 待验收：当前工作机为 `x86_64`，不能按新增脚本要求在 RISC-V/LoongArch 原生 KVM 上运行 schedbench 与 CPU 性能验收；双架构 QEMU 构建、final 小门槛、ext4 并发和官方两题完整回归仍需继续执行。

## 2. 题目对内核的实际要求

官方 `testsuits-for-oskernel` 的 `final-2026` README 已明确：两道题都在 RISC-V64、LoongArch64 上运行，均只测试 glibc。

### 2.1 CAgent

10 个测试只要求基础命令、时间、系统信息、网络状态和简单文件系统操作：

| 测试 | 直接依赖的内核能力 | 当前判断 |
| --- | --- | --- |
| factorial | glibc 动态加载、exec、进程退出、标准输出 | 基础已有 |
| date | `clock_gettime`/`gettimeofday`、时钟和时间格式依赖的 proc/文件接口 | 基础已有，时间语义需验证 |
| cpu | CPU 数量的系统视图 | SMP online CPU 视图已完成待验收，仍需用 CAgent 实际命令验证输出契约 |
| kernel | `uname`、`/proc/version` 或类似内核版本视图 | 基础已有，字符串匹配需验证 |
| network | socket 查询、TCP 状态查询、poll/读取 `/proc/net/tcp` 的可能路径 | 高风险，路径未闭环 |
| fs-create | `openat`、创建、写入、关闭、stat | 基础已有，需数据一致性验证 |
| fs-readwrite | `write/read` 或 `pwrite/pread`、文件偏移和缓存 | 基础已有，需验证偏移/缓存 |
| fs-directory | `mkdirat`、目录项、`getdents64`、stat | 基础已有，需验证目录项一致性 |
| fs-usage | `statfs`/`fstatfs` 和文件系统统计 | 当前明确有固定伪造值 |
| fs-search | 递归目录遍历、`.sh` 文件匹配、`getdents64`、路径解析 | 高风险，已有 find/目录遍历历史风险 |

因此，CAgent 的内核缺口不是“没有文件系统或 Linux ABI”，而是：

1. `statfs` 不能长期返回固定统计值；
2. 网络状态到底由 socket 查询还是 `/proc/net/tcp` 提供，必须按实际用户态命令闭环；
3. 时间、内核版本和其余 `/proc` 视图必须与实际内核状态一致；CPU online 视图已完成待验收；
4. ext4 目录遍历和简单写入必须在 RV/LA 两架构上通过真实 glibc 测试。

### 2.2 BuildStorm

BuildStorm 要在 `-smp 8 -m 8G` 下运行 Debian glibc、Rust/Cargo、数百 crate 的并发编译。对内核的核心压力是：

- 大量 `clone/clone3`、线程创建、TLS、`clear_tid`、futex、信号和子进程等待；
- 大量 `mmap/mprotect/mremap/madvise/brk`、缺页、文件映射和地址空间复制；
- 大量源码/中间产物的 open/read/write/pread/pwrite/statx/getdents/rename/unlink/ftruncate/fallocate/fsync；
- 多核下调度、锁、IPI/中断、设备访问和文件系统缓存并发；
- 8G 物理内存下的页表、页引用计数、页回收、内核 heap、page/buffer cache；
- guest 内真实 `/proc/uptime` 计时和 CPU 数量视图。

## 3. 量化判断：还差多少内核能力

### 3.1 CAgent：基础能力大部分已有，缺口集中在语义闭环

从内核源码静态判断：

- 直接可复用的能力面：进程/exec、ELF 动态加载、基础文件操作、目录遍历、socket/TCP/UDP、时钟和 uname，约覆盖 CAgent 的主要依赖面。
- 明确需要修正的能力面：真实 `statfs`、真实 `sysinfo` 与网络连接状态的 Linux 观测接口；多 CPU/proc 视图已完成待验收。
- 必须实测但不能凭静态代码确认的能力面：glibc 启动、date 的时间格式、fsync/缓存可见性、递归 find、socket 工具的实际调用路径。

判断：CAgent 当前更接近“已有底座、约 2 个确定语义问题 + 4 个高风险验证点”，不是从零开始。但在没有双架构实测前，不把它记为已完成。

### 3.2 BuildStorm：8G 硬阻塞已解除，仍有 4 个系统级高风险块

#### 已解除：8G 内存管理容量不足

当前证据：

- 改造前 RV `PHYSTOP` 为 `0xaf000000`、LA 默认 `PHYSBASE + 128MiB`；当前正常路径按 DTB RAM 区间选择 allocator，并已在 `-m 8G -smp 8` QEMU 启动验证。
- 改造前物理页引用计数表 `k_max_refcount_pages = 262144` 只覆盖 1GiB；当前已改为按最终 managed pages 动态分配，8G QEMU 实测 RV `1751684` 页、LA `1947687` 页。
- 改造前 Buddy tree 固定预留 `BSSIZE = 320` 页；当前已改为显式 tree storage 和动态 metadata，并已通过 8G 启动、跨核 COW/TLB 专项。
- 内核 heap/共享内存仍有目标容量上限，但 PMM 已把 heap metadata、allocator 和 shm 从实际 allocator 区间中显式切分；这不等于已经完成 8G 用户压力验收。

这部分代码缺口已完成收尾；后续仍需用 Cargo/ext4 长压力验证页回收、缓存和文件系统并发，不再把 8G PMM 容量本身列为硬阻塞。

#### 已完成并实测：SMP 8 基础内核能力

本轮已落地并通过双架构实测的能力：

- `NCPU/NUMCPU` 扩展到 8，Makefile 的 `QEMU_SMP` 参数化；QEMU 可按 1/2/4/8 vCPU 启动。
- 从 DTB 建立 possible/online CPU 拓扑；RISC-V 使用 OpenSBI HSM 启动 secondary hart，LoongArch 使用 QEMU IOCSR mailbox/IPI 启动 secondary CPU。
- boot hart 与 secondary hart 分层：全局 PMM/VMM/VFS/设备/进程对象只由主核初始化；各次核各自初始化 CPU 槽位、trap、timer 和中断路径，全部 online 后再统一放行 scheduler。
- RISC-V PLIC 改为按当前 hart 初始化和 claim/complete；LoongArch 使用 CSR_CPUID 作为内核 CPU 槽位来源，避免用户 TLS 污染内核 CPU 标识。
- scheduler 建立 PCB `_running_cpu` 单执行者不变量、每核扫描游标、affinity 过滤、粘附运行核和新任务轮转初始选核。后者参考 Starry 近期 run-queue locality 思路，避免未绑核 pthread 在创建阶段全部堆在父线程所在 CPU。
- CLONE_VM 线程使用 PCB 槽位专属 user trapframe VA；页表更新由全局锁保护，LoongArch 返回用户态时按当前核失效相应 TLB 项。
- `/proc/cpuinfo`、`/sys/devices/system/cpu`、`/proc/<pid>/status`、`sched_getaffinity`、`sched_setaffinity` 和 `getcpu` 统一以 online CPU 集合为准。

本阶段已包含通用跨 CPU 页表修改/TLB shootdown 的双架构专项；它仍不能替代 VFS/ext4/bcache/网络的长时间并发锁审计，以及 Cargo 的多进程压力。

#### 高风险块一：线程、clone、futex、信号和等待

F7LY 不是没有这些能力：

- `clone()` 已处理 `CLONE_VM`、`CLONE_FILES`、`CLONE_SIGHAND`、`CLONE_SETTLS`、`CLONE_CHILD_CLEARTID`、`CLONE_VFORK` 等路径（`kernel/proc/proc_manager.cc:2233-2332`、`2511-2655`）。
- `ProcessMemoryManager::share_for_thread()`、`clone_for_fork()` 和 VMM COW 处理已经存在（`kernel/proc/process_memory_manager.cc:973-1113`、`kernel/mem/virtual_memory_manager.cc:1342-1733`）。
- futex 已有按物理页键匹配、超时、requeue、robust list 清理和 wakeup（`kernel/proc/futex.cc:82-98`、`300-494`）。

但仍有风险：

- `clone3()` 明确对若干 namespace flag 返回 `ENOSYS`，且函数仍带 `TODO("TBF")`（`kernel/sys/syscall_handler.cc:15788-15915`）；必须确认 Rust/Cargo 使用的 flags 落在已支持子集。
- `Pcb::_tid` 注释仍标注多线程支持 TODO（`kernel/proc/proc.hh:127-131`），说明线程身份、线程组和清理路径仍需压力验证。
- scheduler 使用全局进程表扫描和全局锁；单核能运行不代表 8 核下不会出现丢唤醒、死锁、重复回收或调度饥饿。
- Starry 自编译资料曾明确遇到信号传递后未 `wake_task` 导致 cargo 子进程挂起；F7LY 必须用 cargo 的 build script/proc-macro/并发线程场景验证同类问题。

结论：这是“实现存在但不具备 BuildStorm 可信度”的高风险块，不应直接判为缺失，也不能判为完成。

#### 高风险块二：mmap、缺页、COW 和页回收

F7LY 已有 `brk`、`mmap`、`mprotect`、`mremap`、`madvise`、文件映射、COW 页标记和缺页路径（例如 `kernel/sys/syscall_handler.cc:6415-6518`、`15133-15705`，以及 `kernel/mem/virtual_memory_manager.cc` 的 COW 代码）。

真正的差距在重负载容量和回收：

- 8G 页管理元数据当前硬限制见上节；
- Rust 编译会同时产生大量匿名页、文件页、编译中间产物和短生命周期映射；
- 当前没有像 Starry 那样已有明确的“文件干净页回收 + 分配失败重试”自编译证据；
- VMA 仍有固定 `NVMA = 256` 槽位（`kernel/proc/process_memory_manager.hh:47-52`），需要确认 rustc 单进程映射数量不会触顶；
- 多核下 COW 引用计数、页表修改和 TLB 刷新需要跨 CPU 正确同步。

结论：基础 VM 能力已有，但 BuildStorm 需要的是“8G + 大量映射 + 内存压力下稳定回收”，目前属于高风险能力块。

#### 高风险块三：ext4、buffer cache、元数据并发和写回

CAgent 的小文件操作在内核功能上已有较好基础：`openat`、`read/write`、`pread/pwrite`、`mkdirat`、`getdents64`、`renameat2`、`ftruncate`、`fallocate`、`fsync/fdatasync`、`copy_file_range`、`splice` 等函数都能在 `kernel/sys/syscall_handler.cc` 中找到实现。

但 BuildStorm 的问题不只是 syscall 是否存在，而是几十万次并发文件操作能否保持一致性和性能：

- F7LY 使用 lwext4/buffer cache，存在 dirty buffer、LRU、flush 和写回路径（`kernel/fs/lwext4/ext4_bcache.cc:89-307`、`kernel/fs/lwext4/ext4_blockdev.cc:169-242,407-430`）；
- F7LY 自己的 ext4 VFS 文件仍有“RV 上 find 长时间卡住、LA 上 bcache 树损坏”的历史风险注释（`kernel/fs/vfs/vfs_ext4_ext.cc:34`），且存在锁 TODO（`kernel/fs/vfs/vfs_utils.cc:4849`）；
- `statfs` 当前返回固定值（`kernel/sys/syscall_handler.cc:3321-3338`），不能作为真实文件系统容量能力；
- 8G 构建会使 page cache、buffer cache 和内核 heap 同时受压，现有 `NBUF=1024` 只是块缓存数量，不等于具备 Linux 级页缓存回收和并发写回。

结论：CAgent 的文件操作可能只需修正少量语义；BuildStorm 需要对 ext4 元数据、cache 锁顺序、脏块回写、目录遍历、并发 close/rename/unlink 做专项稳定性验证和优化。

#### 高风险块四：Linux 系统信息、时间和网络观测语义

当前确定问题：

- `sys_sysinfo()` 将 uptime、负载、内存、进程数等字段全部填 0（`kernel/sys/syscall_handler.cc:11188-11222`）。
- `statfs()` 通过路径存在性检查后调用 `fill_default_statfs()`，返回固定 ext4 统计值（`kernel/sys/syscall_handler.cc:12243-12298`、`3321-3338`）。
- `/proc/self/status`、`/proc/cpuinfo`、`/sys/devices/system/cpu`、affinity 和 `getcpu` 已改为基于 online CPU 集合输出；仍需以 CAgent/BuildStorm 的实际工具确认格式与调用路径。
- syscall 绑定表中虽然有 socket 家族，但注释仍标注 TODO；实际 `sys_socket()`、`listen()`、`accept()`、`connect()`、`sendto()`、`recvfrom()` 等实现已存在，必须以用户态工具实际路径验证，不能只看注释或函数名。
- 静态搜索没有确认到完整 Linux `/proc/net/tcp` provider。CAgent 的 network 测试可能依赖 socket 查询，也可能依赖 netstat/ss 读取 proc，必须在真实 glibc 工具上确认。

这些问题对 CAgent 直接相关，对 BuildStorm 中 `cores`、`/proc/uptime`、调度和工具链环境检查也相关。尤其不能通过篡改 uptime 获取编译时间分。

## 4. 与 Starry 的内核能力对照

Starry 的参考价值不是“代码更多”，而是它已经为自编译负载处理过同一类内核问题：

| 能力 | Starry 资料中的证据 | F7LY 当前差距 |
| --- | --- | --- |
| 多核和自编译参数 | 有 QEMU SMP、jobs、guest runner 参数化流程；部分历史配置仍需区分当前入口 | F7LY 已完成 1/2/4/8 核启动、绑核 pthread、RV stress-ng、CPU 视图和双架构 TLB 专项；仍缺 Cargo/ext4 长时间并发 |
| 8G 内存 | 曾修复 FDT 内存识别、bitmap 容量和动态物理区间 | F7LY 已移除 PHYSTOP、引用计数表和 buddy metadata 的 1GiB 硬限制，并通过 RV/LA 8G QEMU 验收 |
| 大编译页压力 | 曾增加文件页回收和分配失败重试 | F7LY 尚无同等级 BuildStorm 实测，page/buffer cache 压力未知 |
| cargo 子进程 | 曾修复信号传递后唤醒任务、`dumpable/no_new_privs` 等工具链依赖 | F7LY 有信号/futex/clone，但需用 cargo build script 和 proc-macro 验证 |
| 文件系统 | 曾遇到 ext4 metadata_csum、SMP 写死锁、tmpfs mount API 等问题 | F7LY lwext4 也有目录遍历卡顿、bcache 损坏和锁风险记录 |
| syscall 组织 | 按 task、futex、signal、mm、fs/fd、epoll、timerfd、memfd 等模块拆分并配有系统测试 | F7LY 集中在 `syscall_handler.cc`，很多接口虽有实现但缺少针对本题的最小回归矩阵 |

Starry 的经验说明：F7LY 当前最需要补的是“容量、并发、回收、语义一致性”的系统能力，而不是简单增加 syscall 数量。

## 5. 内核专项实施计划

### P0-0：final-2026 小门槛兼容收尾

- [x] 代码完成待验收：移除会遮挡真实文件的空 `/etc/ld.so.cache`、`/etc/ld.so.preload`、`/etc/localtime` 虚拟实现。
- [x] 代码完成待验收：`/etc/passwd`、`/etc/group`、`/etc/hosts`、`/etc/resolv.conf`、`/etc/protocols` 改为真实 backing 优先；虚拟节点只在镜像缺失真实文件时作为后备。
- [x] 代码完成待验收：新增 `vfs_backing_path_exists()` 和 `vfs_resolve_runtime_interpreter()`，`execve` 只解析 ELF `PT_INTERP` 并通过统一路径解析器定位真实动态链接器；旧 `/glibc`、`/musl` 兼容别名只在真实标准路径缺失时启用。
- [x] 代码完成待验收：实现动态 `/proc/uptime`，`sysinfo.uptime` 同步来自 `CLOCK_BOOTTIME`；未做时间缩放或伪造。
- [x] 代码完成待验收：PCB 保存最近一次成功 `execve` 的 NUL 分隔 argv，`/proc/self/cmdline` 动态读取该状态，fork/clone 继承，失败 exec 不覆盖旧值。
- [x] 代码完成待验收：`uname.machine` 按编译架构返回 `riscv64` 或 `loongarch64`。
- [x] 已收尾：清除 `CARGO_SYSCALL_*`、`BUILDSTORM_EXEC_*`、`BUILDSTORM_CLONE_*`、`BUILDSTORM_FIRST_*` 临时串口诊断输出；这些输出原本用于定位 Cargo/BuildStorm 在 exec、clone、首次调度和阻塞 syscall 上的卡点，定位完成前若还需要，应改为短期局部补丁，不应留在最终回归路径。
- [x] 已收尾：删除开发过程中的 final 小门槛烟测入口，不在 `user/user_lib/user_test.cc` 和 `user/deps/user.hh` 中保留临时测试代码。
- [ ] 待完成：运行小门槛烟测并记录双架构结果：`uname -m`、真实 `ld.so.cache` 大小非零、`/etc/localtime` 符号链接、`/bin/bash -c true`、`rustc --version`、`cargo --version`、最小 Cargo 项目构建运行、`/proc/uptime` 两次读取严格递增。

### P0-A：先解除 8G 内存硬阻塞

- [x] 从 DTB/平台内存描述建立 RV/LA 真实物理内存区间模型，排除 kernel、DTB、initrd 和空洞，并已在 8G QEMU 启动验证。
- [x] 将 `k_max_refcount_pages` 从 1GiB 固定数组改为按 managed pages 动态分配；RV/LA 已分别实测 `1751684`/`1947687` 个 managed pages。
- [x] 将 Buddy tree 元数据按实际页数动态预留，移除 `BSSIZE=320` 对 8G 的硬限制。
- [x] 重新核对内核 heap、共享内存、页表和 PMM metadata 的地址布局；跨核 COW/TLB 专项已通过，buffer/page cache 压力仍需 Cargo/ext4 长压力验证。
- [x] 验证 8G 启动、动态 PMM 容量和跨 CPU COW/TLB 失效专项；1G/4G 全量分配/OOM 矩阵仍可作为后续补充。

验收：`-m 8G` 能启动并完成大规模匿名 mmap、clone/fork、文件映射和释放，不在 PMM、buddy、refcount、页表或 heap 处 panic。

### P0-B：建立真正的 SMP 8 内核

- [x] 代码完成待实测：将 `-smp`、`NCPU/NUMCPU` 和 CPU mask 参数化，并实现 RV64/LA64 的 1/2/4/8 核路径。
- [x] 代码完成待实测：将 boot hart 与 secondary hart 两套初始化阶段迁移到 `kernel/hal/<arch>/smp.cc`，确保 PMM/VMM/VFS/设备/进程全局对象只初始化一次。
- [x] 代码完成待实测：完成每核 trap、timer、PLIC/APIC、IPI、设备中断 claim/complete，消除 RISC-V PLIC 对 hart 0 的硬编码；CLONE_VM trapframe 映射按线程隔离。
- [x] 代码完成待实测：为 scheduler、SpinLock、sleep/wakeup 和 affinity 建立多核执行权不变量及初始选核策略。
- [x] 代码完成待实测：让 `/proc/cpuinfo`、`Cpus_allowed`、`sched_getaffinity`、`getcpu` 与 `/sys` CPU 拓扑反映同一个 online CPU 集合。
- [x] 代码完成待实测：实现共享用户页表修改场景的通用跨 CPU TLB shootdown；VFS/ext4/bcache/网络的长时间锁顺序审计仍未完成。
- [ ] 待完成：运行至少 30 分钟的并发线程/进程压力与 Cargo 实际构建，覆盖死锁、丢唤醒、非法 CPU 索引、重复初始化和设备中断异常。

当前工作树的 CPU、stress-ng 和 TLB 结果及复现命令统一记录在 `docs/dev-notes/smp_buildstorm_reproduction.md`；本轮临时日志已清理，不在计划中保留已删除的日志路径。短压力结果不替代下面的 30 分钟与 Cargo 验收门槛。

### P0-C：用 Rust 最小构建验证进程和内存语义

- [ ] 待完成：使用官方 runner 或一次性手工命令确认 Rust/glibc 小项目能在官方镜像真实文件布局下构建运行；该检查只证明工具链基础可启动，不代表 BuildStorm 长压力通过，不能把临时测试入口写入 `user_test`。
- [ ] 验证 `clone/clone3` 的 Rust/Cargo 实际 flags，包括 `CLONE_VM`、`CLONE_FS`、`CLONE_FILES`、`CLONE_SIGHAND`、`CLONE_SETTLS`、`CLONE_CHILD_CLEARTID` 和 `CLONE_PARENT_SETTID`。
- [ ] 验证线程 tid/tgid、TLS、clear_tid、线程退出、wait、robust futex、信号中断和父子回收。
- [ ] 验证 cargo build script、proc-macro、并发 rustc 子进程不会因 wakeup、管道、epoll 或 futex 丢失而挂死。
- [ ] 记录实际 syscall 轨迹；只修复出现的语义缺口，不依据绑定表盲目扩展不相关接口。

验收：glibc Rust 工具链可完成 `cargo new`、`cargo build` 和 Hello World，多次运行结果稳定。

### P0-D：schedbench 与 SMP 性能验证

- [x] 代码完成待验收：`tools/smp/schedbench.c` 已作为专项调度基准加入 `tools/smp/`；保留其原始 wake、broadcast、independent、wave 校验逻辑和 `SCHEDBENCH_*` 输出标记。
- [x] 脚本完成待运行：新增 `scripts/run/schedbench.sh`，静态交叉编译双架构 bench，只向 `/tmp/f7ly-schedbench-*` 可丢弃镜像副本注入 `/schedbench`，运行前后校验 `images/` 下官方镜像 SHA-256 未变化，退出时清理临时镜像和二进制。
- [x] 脚本完成待运行：`scripts/run/smp_cpu_bench.sh` 改为 native KVM 性能验收入口，默认 8 GiB、8 vCPU、1/2/4/8 worker、每组 3 轮，用中位吞吐计算 4/8 worker 至少 1.10 倍的门槛。
- [ ] 待完成：在 riscv64 原生 KVM 宿主运行 `scripts/run/schedbench.sh --arch rv`，正式参数为 `workers=8 cpus=8 warmup=256 rounds=4096 migration-pattern=both working-set-kib=256 runs=3`。
- [ ] 待完成：在 loongarch64 原生 KVM 宿主运行 `scripts/run/schedbench.sh --arch la`，同样连续 3 轮。
- [ ] 待完成：确认 fanout、broadcast、independent、wave 全部出现 `SCHEDBENCH_CASE_PASSED`，最终出现 `SCHEDBENCH_PASSED`，错误字段均为 0，8 个 CPU 均被观测到。
- [ ] 待完成：在两类原生宿主分别运行 `scripts/run/smp_cpu_bench.sh --arch rv` 和 `scripts/run/smp_cpu_bench.sh --arch la`，确认 4/8 worker 中位吞吐达到 1.10 倍；当前 `x86_64` 工作机不能执行该 KVM 验收。

### P1-A：补强 VM、缺页、COW 和回收

- [x] 代码完成待验收：同一 `CLONE_VM` 地址空间的 `mmap`、`mprotect` 和用户缺页路径已统一进入可睡眠内存锁；内存锁支持同线程重入，避免 copyin/copyout 懒补页重入死锁。
- [x] 代码完成待验收：匿名私有 mmap 合并后会重建 Maple Tree 索引，避免扩展尾部无法被后续 `mprotect`/缺页查到。
- [x] 代码完成待验收：`brk` 扩展不再穿过任意已有 VMA，防止堆覆盖动态库或文件私有映射。
- [ ] 验证匿名/文件 mmap、懒分配、写时复制、mprotect 权限变化、mremap、madvise、brk 和 `munmap` 的组合行为。
- [ ] 在大内存编译压力下增加或验证干净文件页回收、分配失败重试和 cache 驱逐，防止 rustc 因 page cache 吞噬可用内存而失败。
- [ ] 检查 VMA 数量上限、地址空间碎片、页表回收、引用计数和多核 TLB 一致性。
- [ ] 对 1G/4G/8G、单进程/多线程/多进程分别记录峰值内存和失败位置。

验收：完整构建期间无 OOM panic、页表损坏、COW 数据串扰、用户页权限错误或长期回收抖动。

### P1-B：补强 ext4/bcache 并发稳定性

- [x] 代码完成待验收：lwext4 挂载锁已替换为线程 PCB 所有权的可重入 FIFO 睡眠锁，且 mount lock 先安装、write-back cache 后开启。
- [x] 代码完成待验收：新增 `Ext4MountGuard`，已覆盖 `fstat/statx/truncate/utimens/ioctl inode flags/normal_file` 等直接调用 `ext4_fs_get_inode_ref()` 的 VFS 低层路径。
- [x] 代码完成待验收：bcache dirty list 改为双向 TAILQ，插入/删除为 O(1)，重复释放、活引用回收、链表指针损坏改为现场断言，不再吞掉 dirty-list 或引用计数错误。
- [x] 代码完成待验收：virtio-blk 容量改为读取设备真实 capacity，不再固定 4 GiB；`getdents64` 使用文件系统目录 cookie 更新 `_file_ptr`；`O_TMPFILE` 后备隐藏目录项在最后关闭时删除。
- [ ] 用 CAgent 的创建、读写、目录、搜索用例验证基础 ext4 语义，重点检查 close/reopen、目录项刷新、文件偏移和缓存可见性。
- [ ] 用并发 Cargo 构建验证 inode、目录、extent、journal、dirty buffer、LRU、flush/writeback 和 block device 的锁顺序。
- [ ] 优先处理已记录的 RV `find` 卡顿、LA bcache 树损坏和 VFS 锁 TODO；每个问题保留最小复现和回归测试。
- [ ] 审核 `pread/pwrite/ftruncate/fallocate/fsync/fdatasync/renameat2/unlinkat/getdents64` 的返回值、部分读写和并发关闭语义。
- [ ] 将 `statfs` 改为从实际挂载文件系统统计，至少使 blocks/free/available 与测试镜像一致。

验收：CAgent 文件项双架构通过；BuildStorm 运行期间无文件系统损坏、目录遍历永久阻塞、dirty cache 丢失或 journal 错误。

### P1-C：统一 Linux 信息、时间和网络状态语义

- [x] 代码完成待验收：`/proc/uptime` 和 `sysinfo.uptime` 已来自 `CLOCK_BOOTTIME`。
- [ ] 补齐 `sysinfo` 的进程数、负载和更完整 Linux 语义；内存字段与 `mem_unit` 已改为读取动态 PMM，仍需和 `/proc/uptime` 一并验收。
- [ ] 校准 `clock_gettime`、`gettimeofday`、nanosleep、futex timeout、timerfd 和 `/proc/uptime` 的单调时间关系，并确认 BuildStorm 用真实时间计分。
- [x] 已完成待验收：修正 `/proc/cpuinfo`、`/proc/self/status`、`/sys/devices/system/cpu`、affinity 和 `getcpu`，消除 CPU 视图的单核硬编码。
- [x] 代码完成待验收：`uname.machine` 按编译架构返回 `riscv64`/`loongarch64`。
- [ ] 待完成：验证 uname/version 与 CAgent 的实际字符串契约，尤其是 kernel/version 测试是否读取 `/proc/version` 或其它节点。
- [ ] 使用 CAgent 实际依赖的 glibc 工具确认 TCP 连接数的查询路径；若依赖 `/proc/net/tcp`，补齐真实 socket 状态 provider。

验收：CAgent 的 date、cpu、kernel、network、fs-usage 均不依赖固定伪造值，且不会破坏 BuildStorm 的真实 guest 计时。

### P2：BuildStorm 性能优化

- [ ] 先取得内核能跑通的成功基线，再拆分 syscall、调度、缺页、文件缓存、ext4 元数据和 block I/O 耗时。
- [ ] 优先优化全局锁、全表扫描、重复用户/内核拷贝、目录查找、脏块写回和 page cache 抖动。
- [ ] 每项优化保留正确性回归和多次耗时数据，不能通过跳过构建、复用旧 target 或修改 uptime 获取时间分。

## 6. 内核能力验收门槛

### CAgent 内核门槛

- [ ] RV64/glibc 和 LA64/glibc 的 10 个测试均能启动并完成；
- [ ] `statfs`、CPU 数量、kernel version、时间和 TCP 状态来自真实内核状态；
- [ ] 文件创建、读写、目录遍历和搜索在重启/关闭文件后仍保持一致；
- [ ] 日志中无 syscall panic、无长期卡死，且每项耗时满足官方超时。

### BuildStorm 内核门槛

- [x] `-smp 8 -m 8G` 稳定启动，8 个 CPU 在 guest CPU 视图中一致；
- [x] PMM、buddy、refcount、页表和 COW/TLB 专项覆盖 8G，不使用 1GiB 固定上限；
- [ ] cargo build script、proc-macro、多线程 futex/信号/等待链路稳定；
- [ ] 完整 `cargo xtask arceos build` 能从零完成，产物不小于 500KB；
- [ ] 构建过程无内存 panic、文件系统损坏、死锁、丢唤醒、页表错误和设备中断错误；
- [ ] `/proc/uptime` 和 `cores` 为真实内核状态，能够用于官方耗时判定。

## 7. 当前不应误判的地方

- F7LY 已实现 `pread/pwrite`、socket、epoll、mmap、COW 等函数，所以不能简单把 syscall 绑定表中的 TODO 注释全部当作“未实现”；应检查实际返回值和语义。
- 反过来，函数存在也不能当成 BuildStorm 已支持；8G 页容量、SMP 中断、缓存锁和并发回收是 syscall 之上的系统能力。
- `sys_fork`/旧 `sys_exec` 返回 `ENOSYS` 不一定直接阻塞 Rust，因为当前 glibc 可能通过 clone/execve 路径运行；但必须由实际工具链 syscall 轨迹确认，不能凭猜测排除。
- CAgent 的 `cpu` 只要求输出数字，单核下返回 1 可能满足脚本格式；但这不等于 F7LY 已具备 BuildStorm 所需的 8 核语义。
- Starry 的自编译成功记录不能直接等价为 F7LY 成功；它只能证明上述内存、信号、文件缓存和 ext4 问题确实是复杂自编译负载的典型阻塞点。

## 8. 参考资料和代码证据

官方规则：

- [testsuits-for-oskernel `final-2026`](https://github.com/oscomp/testsuits-for-oskernel/tree/final-2026)
- [final-2026 README](https://github.com/oscomp/testsuits-for-oskernel/blob/final-2026/README.md)

Starry 参考：

- `~/tgoskits/os/StarryOS/docs/starryos-self-compilation.md`
- `~/tgoskits/scripts/prepare-selfhost-rootfs.sh`
- `~/tgoskits/apps/starry/macos-selfbuild/prebuild.sh`
- `~/tgoskits/apps/starry/macos-selfbuild/guest-selfbuild.sh`
- `~/tgoskits/os/StarryOS/kernel/src/syscall/mod.rs`

F7LY 重点证据：

- `kernel/mem/memlayout.hh:67-71,123-127`：RV/LA 默认物理内存上限；
- `kernel/mem/physical_memory_manager.cc`、`kernel/mem/buddysystem.cc`：改造前的 1GiB 引用计数上限和固定 Buddy metadata 容量；
- `kernel/libs/param.h`、`Makefile`：8 核容量与 `QEMU_SMP` 参数；
- `kernel/boot/riscv/main.cc`、`kernel/boot/riscv/start.cc`、`kernel/boot/loongarch/main.cc`、`kernel/hal/loongarch/smp.cc`：双架构 secondary CPU 启动与 bootstrap gate；
- `kernel/trap/riscv/plic.cc`、`kernel/trap/riscv/trap.cc`、`kernel/trap/loongarch/trap.cc`：每核中断、定时抢占和用户态返回；
- `kernel/proc/scheduler.cc`、`kernel/proc/proc_manager.cc`、`kernel/hal/cpu.cc`：SMP 调度执行权、初始选核与 online CPU 拓扑；
- `scripts/run/smp_cpu_bench.sh`、`tools/smp/f7ly_smp_cpu_bench.c`：双架构 CPU 压测与可复现验收；
- `kernel/proc/proc_manager.cc:2233-2655`、`kernel/proc/futex.cc:82-494`：已有 clone/线程/futex 基础；
- `kernel/mem/virtual_memory_manager.cc:1342-1733`：已有 COW 处理；
- `kernel/sys/syscall_handler.cc:3321-3338,11188-11222,12243-12298`：statfs 仍有固定统计值；sysinfo 内存字段已切换到动态 PMM，uptime/进程数语义仍待验收；
- `kernel/fs/vfs/file/virtual_file.cc`：按 online CPU 集合生成 proc/sys CPU 视图；
- `kernel/fs/vfs/vfs_ext4_ext.cc:34`、`kernel/fs/vfs/vfs_utils.cc:4849`：ext4/find/bcache 和锁风险注释；
- `kernel/fs/lwext4/ext4_bcache.cc:89-307`、`kernel/fs/lwext4/ext4_blockdev.cc:169-242,407-430`：当前 buffer cache/LRU/写回路径。

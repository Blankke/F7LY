# 2026 决赛内核能力缺口计划：CAgent / BuildStorm

状态：LoongArch BuildStorm 完整正式构建已通过、CAgent 并发结果行已稳定；重复长测和 Docker harness 双架构 workflow 按用户边界留待外部验收

日期：2026-07-29

## 1. 范围和结论

本计划只评估“内核是否具备支撑两道题的能力”，不把 rootfs 制备、测试 runner、串口标记、judge 和源码注入算作内核缺口。它们只是后续验证这些内核能力的前提。

结论如下：

- [x] 2026-08-01 全量代码收口待验收：实现真实 ext4 `statfs/fstatfs`、动态 `sysinfo` 进程数与 1/5/15 分钟负载、真实 `/proc/net/tcp{,6}` 状态快照、epoll 零超时无分配快路、O(NUMCPU) 初始选核压力记账和语义化 SMP 唤醒 IPI；完成 ext4 组件缓存、bcache 热点、relatime、COW/TLB、`has_resident_pages`、clone3/线程清理的高风险审计。
- [x] 2026-08-01 双架构定向回归完成待验收：RV/LA 均在 4G/8 vCPU 下执行 78 次 glibc LTP，66 项返回 0、12 项为环境型 TCONF，0 TFAIL/TBROK，QEMU 自然退出 0。`statfs02`、`fstatfs02`、`epoll_wait04` 连续 10 次、clone301/302/303、futex、mmapstress03/04/05、shmget05/06、时间、fsync/fallocate、pread/pwrite、link/symlink/rename/unlink/getdents 均无内核失败；证据为 `logs/run/output_final2026_{rv,la}_targeted_highrisk_fixed_4g8c.txt`。
- [x] 2026-08-01 新官方镜像已替换：RV/LA `sdcard-*-pub.img` 均为 15032385536 字节，压缩包 SHA-256 分别为 `cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1` 和 `2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2`。正式入口已统一恢复为 `cagent_test(); buildstorm_test();`，四种构建均通过。
- [x] 2026-08-01 第二轮失败路径审计及回归完成待验收：修复 clone OOM/fd/sighand 失败时 PCB 漏解锁、共享 `CLONE_VM` 被错误紧急清理、RUNNABLE 子任务回收 panic、共享 fd/sighand 引用竞态与 POSIX 锁提前释放；futex/socket 登记锁改为 SMP 一次性初始化，socket `/proc` 登记改为无容量上限的内嵌链；epoll 零超时对任意 `maxevents` 均不分配页，并以轮转游标保持 LT 公平和 ET 未交付事件；load average 改为 CPU0 每 5 秒真实采样。四种构建通过；最新 RV/LA 4G/8 vCPU 窄回归中，clone301/302、`epoll_wait04` 连续 10 次和 4 个 futex 用例全部返回 0，clone303 仅因镜像缺少 cgroup v2 返回 TCONF，0 TFAIL/TBROK/panic；证据为 `logs/run/output_final2026_{rv,la}_clone_epoll_futex_reaudit_4g8c.txt`。
- [x] 2026-08-01 定时器与 epoll 抖动收口待验收：定时中断在没有已武装 ITIMER/POSIX timer 时不再扫描 PCB 或全局定时器表；POSIX timer 的创建、设置、查询、删除、到期和进程退出清理由同一锁保护，补齐归属校验、`timer_getoverrun`、EFAULT 和周期定时器 O(1) overrun 计算。RV/LA 的 timer_delete/gettime/settime、clock_settime、getitimer 与 `epoll_wait04` 连续 10 次共 19 项全部返回 0；证据为 `logs/run/output_final2026_{rv,la}_posix_timer_all_fastpaths_4g8c.txt`。
- [x] 2026-08-01 CAgent 9/10 根因修复完成待验收：并发唤醒先原子筛选目标 channel，再只获取真实睡眠候选 PCB 锁，消除扫描无关 PCB 导致的跨核 ABBA；普通文件最后关闭时把合并写提交到 ext4/bcache，并删除容量受限、可能让已关闭文件回退到旧 inode EOF 的全局小文件缓存。两项均位于双架构共享实现，没有架构特判或测例绕过；RV/LA evaluation 构建通过，官方原始 CAgent 各连续三轮 10/10，诊断轮十项首次校验均为 0。证据为 `logs/run/output_cagent_{rv,la}_spinlock_fix_final{1,2,3}_20260801.txt` 和 `logs/run/output_cagent_rv_eval_raw_closeflush2_20260801.txt`。
- [x] 2026-08-01 BuildStorm `Compiling core` futex 死锁修复完成待验收：修复前 GDB 证实一个 CPU 持全局 futex 等待锁后等待目标 PCB 锁，另一个 CPU 持当前 PCB 锁后等待全局 futex 锁；WAIT 现统一按“全局 futex 锁 -> PCB 锁”取得锁，并在该互锁内重新读取用户值后才发布原子 futex key，既消除 ABBA，也不引入丢唤醒窗口。共享实现同时覆盖 RV/LA；两架构 evaluation 构建及 `futex_wait01`、`futex_wait04`、`futex_wait_bitset01`、`futex_wake01` 小集合全部通过。修复后同一 RV 8 vCPU 内层构建在 `Compiling core` 处的 GDB 快照不再出现 futex 自旋闭环；完整 BuildStorm 长测仍由用户验收，不在此项中冒充完成。
- [x] 2026-08-08 BuildStorm `core` 阶段远端 TLB 等待根因修复完成待验收：修复前 8 vCPU 三份间隔 20 秒诊断快照显示远端 TLB shootdown 调用与累计等待持续增长，且 1 vCPU 不再触发远端调用；修复后 ASID 随 `ProcessMemoryManager` 生命周期管理，`CLONE_VM` 线程共享 mm/ASID，用户页表更新只同步活跃 mm CPU。RV/LA evaluation 与 shell 四种构建及 8 vCPU/8 GiB/20 轮 TLB/VMA 短测均通过；完整 BuildStorm 仍由用户执行，不在此项中冒充完成。
- [x] 2026-08-08 BuildStorm `core` 阶段 ext4 剩余性能根因已完成代码修复待验收：诊断显示 ext4 ticket/owner 正常推进但全局挂载锁等待和累计周期占主要成本；读路径现使用共享挂载锁，bcache 单独串行化，完整块写入改为 write-back，直接块 I/O 移出全局排他临界区，并修复同 owner 嵌套 mount guard。双架构 8 vCPU/8 GiB 并发 `write/fsync/rename/read/unlink` 短测与临时镜像只读 `e2fsck` 均通过。
- [x] 2026-08-09 有界官方进度探针已读取完整结果：最终受控日志 `logs/run/output_r_20260809-073601_buildstorm-perf.txt` 在 8 vCPU、8 GiB、guest 1200 秒内实际输出 `Compiling compiler_builtins` 与 `Compiling core v0.0.0`，产物计数曾达到 27，但未输出后续 `Compiling`，退出 `rc=124`；这证明“仅 Cargo/rustc 存活或产物增长”的证据不足，也明确保留完整 BuildStorm 外部验收门槛。
- [x] 2026-08-12 LoongArch BuildStorm 崩溃根因修复完成待验收：普通 TLB 表项覆盖相邻双页，原范围失效从 4 KiB 奇数页起步却按 8 KiB 递增，会漏掉区间末端 pair；现统一把半开区间扩张到双页边界，溢出时保守失效全部非全局项。增强专项在旧内核稳定报告 5/10 次预期 fault、修复后 RV/LA 各 50 轮得到 100/100 fault；LA 8 vCPU/8 GiB 正式 BuildStorm 完整输出 `ok=true`，产物 1714568 字节、guest 耗时 1425.69 秒，无 rustc ICE、panic 或 shootdown timeout，证据为 `logs/run/output_l_20260812-104222_buildstorm-perf.txt`。
- [x] 2026-08-12 CAgent 并发结果误判修复完成待验收：十个后台 agent 的校验实际通过，但 stdout/stderr 原先逐字符获取 UART 锁，会把多个 `testcase cagent ... pass` 行交错成不可解析文本。字符设备新增批量同步写契约，UART 用独立输出事务锁串行化一次 `write()`，同时保留 IRQ 使用原队列锁排空；修复后 LA 连续 5 轮、RV 连续 3 轮均严格得到 10 个唯一项且全部 pass，无 reject、panic 或粘连行。
- [ ] 2026-08-01 外部验收仍开放并由用户执行：完整 BuildStorm 在无其它 QEMU 竞争时串行跑完两轮，恢复 harness 后再跑两轮双架构 workflow；未取得这些长测证据前，不把这些外部项误标为已验收。

- CAgent：F7LY 的基础内核能力已经覆盖较多。动态 ELF/glibc、进程创建与 exec、基础时间、文件创建/读写/目录遍历、socket 和 TCP/UDP 路径都存在，暂时看不到必须重做内核架构的缺口。主要差距是若干 Linux 语义不完整或返回固定值，需要针对 10 个 CAgent 测试逐项验证。
- BuildStorm：动态 8G PMM、SMP HAL、通用 TLB shootdown 和实际 CPU 并行能力已经通过双架构 QEMU 验收；RV 用户基线与 LA 修复后各有一轮完整 Cargo/ext4 长构建 PASS，当前只保留重复轮次和 Docker harness 外部验收。
- 现有 syscall 绑定数量不能作为完成度指标。F7LY 的很多关键 syscall 已有函数实现，但仍带 TODO、固定返回值、错误路径 panic 或只覆盖部分 Linux 语义；应以 Rust 工具链和 CAgent 的真实 syscall 轨迹验收。
- [x] 2026-07-26：官方 `final2.1` 双架构镜像、8 核/8 GiB QEMU 参数和两道 glibc runner 已接入，代码完成待验收。首次实测已暴露 RV ext4 bcache 并发损坏，以及 LA 动态库搜索和 `/work/tgoskits` 访问失败，尚未达到赛题通过门槛。
- [x] 2026-07-26 收尾：移除内核中面向 Cargo/BuildStorm 的临时串口跟踪；LoongArch initcode 恢复为 CAgent + BuildStorm 双题入口；BuildStorm runner 保留 `/bin/sh /glibc/buildstorm_testcode.sh` 调用但去掉 `sh -x` 跟踪，避免污染官方日志。
- [x] 2026-08-01 TCG 范围已收口：本项目官方评测路径即跨架构 QEMU TCG，双架构四种构建、4G/8 vCPU 定向回归和既有 SMP 多核证据已完成；不再把当前工作机无法 native KVM 作为开放项，官方完整两题仍按独立门槛验收。
- [x] 2026-07-29 原阻塞点已解除待验收：RV QEMU 8G/8 vCPU 下官方 BuildStorm 连续打印 `BUILDSTORM_TOOLCHAIN ok`、`BUILDSTORM_MINIBUILD ok`，随后完成依赖图解析并进入 tg-xtask 并发编译，进度从 `0/446` 前进到 `1/446`。进程快照确认 `Resolving dependency graph...` 阶段 Cargo 处于运行态并持续增加 CPU 时间，不是睡眠等待；本轮只验收到“测试正常开始”，不等同于完整 BuildStorm 长构建通过。
- [x] 2026-07-29 LA 正常启动已完成待验收：8G/8 vCPU 下真实 Rust 工具链完成 `cargo new`、`cargo build` 和 Hello World；官方 BuildStorm 也已解析依赖并从 `0/446` 前进到 `1/446`，未宣称完整长构建通过。
- [x] 2026-07-29 LA ASID/TLB 稳定化已完成待验收：内核使用 ASID 0、用户任务使用隔离回收池；连续 3 轮、每轮 8 worker 共 1120 次进程生命周期均通过 ASID 耗尽复用，无 panic 或 shootdown timeout。
- [x] 2026-07-29 RV ASID/TLB 热路径完成待验收：用户页表使用非零 ASID，trap 往返不再执行 4 次全量 `sfence.vma`；8 worker 共 1120 次进程生命周期跨池复用通过，随后官方 BuildStorm 再次从依赖解析进入 `Compiling` 和 `1/446`。
- [x] 2026-07-29 LA CAgent 性能复验已完成待验收：清除另一个 8-vCPU RV QEMU 的外部竞争后，同构诊断脚本 10/10 通过（fs-search 14.706 秒、network 19.610 秒），与历史无竞争官方日志一致。另一次官方脚本复跑为 9/10，fs-search 在 14.247 秒提前结束但输出判定失败；后续原始输出诊断确认这类随机 reject 来自已关闭小文件的延迟可见性，而非 LLM 分类或命令内容，并已由 2026-08-01 的 close 写回语义修复。

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
| network | socket 查询、TCP 状态查询、poll/读取 `/proc/net/tcp` 的可能路径 | 已实现受锁 TCP/TCP6 只读快照，官方 CAgent 双架构连续三轮 10/10 |
| fs-create | `openat`、创建、写入、关闭、stat | 基础已有，需数据一致性验证 |
| fs-readwrite | `write/read` 或 `pwrite/pread`、文件偏移和缓存 | 基础已有，需验证偏移/缓存 |
| fs-directory | `mkdirat`、目录项、`getdents64`、stat | 基础已有，需验证目录项一致性 |
| fs-usage | `statfs`/`fstatfs` 和文件系统统计 | 已改为真实 ext4 挂载统计，RV/LA `statfs02/fstatfs02` 通过；`statfs01` 因镜像无 mkfs 工具为 TCONF |
| fs-search | 递归目录遍历、`.sh` 文件匹配、`getdents64`、路径解析 | 高风险，已有 find/目录遍历历史风险 |

因此，CAgent 的内核缺口不是“没有文件系统或 Linux ABI”，而是：

1. `statfs/fstatfs` 已返回真实挂载统计，并保持路径与 fd 错误码；
2. `/proc/net/tcp{,6}` 已由受锁 socket 快照生成 LISTEN/CONNECTING/ESTABLISHED 等真实状态；
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
- `statfs/fstatfs` 已统一经过 VFS 调用 ext4 mount stats，返回真实 block、inode、fsid、name length 和挂载标志；错误路径由双架构定向 LTP 覆盖；
- F7LY 没有无界全局文件页缓存；BuildStorm 的文件数据压力主要落在固定 8192 项的 lwext4 bcache、脏块写回和内核 heap。缓存容量固定不等于写回与驱逐已验收，仍须由完整构建覆盖分配失败和并发回收路径。

结论：CAgent 的文件操作可能只需修正少量语义；BuildStorm 需要对 ext4 元数据、cache 锁顺序、脏块回写、目录遍历、并发 close/rename/unlink 做专项稳定性验证和优化。

#### 高风险块四：Linux 系统信息、时间和网络观测语义

当前收口结果：

- `sys_sysinfo()` 的 uptime 来自 `CLOCK_BOOTTIME`，进程数来自活动 PCB，负载按 5 秒采样导出 Linux 16 位定点 1/5/15 分钟值，内存来自动态 PMM且 `mem_unit=PGSIZE`。
- `statfs/fstatfs` 已通过统一 VFS 接口读取 ext4 mount stats，返回真实块、可用块、inode、name length、fsid 和挂载标志，不再调用固定填充函数。
- `/proc/self/status`、`/proc/cpuinfo`、`/sys/devices/system/cpu`、affinity 和 `getcpu` 已改为基于 online CPU 集合输出；仍需以 CAgent/BuildStorm 的实际工具确认格式与调用路径。
- syscall 绑定表中虽然有 socket 家族，但注释仍标注 TODO；实际 `sys_socket()`、`listen()`、`accept()`、`connect()`、`sendto()`、`recvfrom()` 等实现已存在，必须以用户态工具实际路径验证，不能只看注释或函数名。
- `/proc/net/tcp` 与 `/proc/net/tcp6` 已由受锁 socket 注册表快照生成 Linux 格式视图，连接数不再固定；CAgent network 已在 RV/LA 官方脚本单轮通过。

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
- [x] 已由官方链路覆盖待连续轮次验收：RV/LA CAgent 运行确认 uname/date/kernel/动态 glibc，BuildStorm 输出 rustc/cargo 版本并通过 toolchain/minibuild；`/proc/uptime` 使用真实单调时钟。新版镜像的真实系统文件保持 backing 优先，不注入临时烟测脚本。

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

- [x] 2026-07-29 完成待验收：官方 runner 在未修改的 RV 镜像中确认 Rust/glibc 小项目可完成 `cargo new`、`cargo build` 和 Hello World，并打印 `BUILDSTORM_MINIBUILD ok`；该检查只证明工具链基础可启动，不代表 BuildStorm 长压力通过，未把临时测试入口写入 `user_test`。
- [x] 2026-07-26 临时 shell 拆解完成：这些测试只用于定位 BuildStorm minibuild 卡点，不是回归入口，不应接入 `user_test`。已确认手工 `rm -rf`、`mkdir -p`、写 `Cargo.toml`、写 `src/main.rs` 都能正常完成。
- [x] 2026-07-26 临时 shell 拆解完成：直接调用真实 toolchain cargo，即 `/root/.rustup/toolchains/nightly-2026-05-28-riscv64gc-unknown-linux-gnu/bin/cargo new --vcs none /tmp/minibuild` 可以返回并生成文件；官方路径 `/root/.cargo/bin/cargo new --vcs none /tmp/minibuild` 会通过 rustup shim 卡住，后台观察到 `[cargo]` 与 `[rustup]` 残留进程。
- [x] 2026-07-26 临时 shell 拆解完成：`/lib/ld-linux-riscv64-lp64d.so.1 --list <toolchain>/bin/rustc` 能按 `DT_RPATH=$ORIGIN/../lib` 找到 `librustc_driver-*.so`；但普通直接 exec `<toolchain>/bin/rustc --version` 会报 `librustc_driver-*.so: cannot open shared object file`，设置 `LD_ORIGIN_PATH=<toolchain>/bin` 后可通过。该环境变量只用于定位，不能作为最终修复。
- [x] 2026-07-29 修复完成待验收：根因是多线程 rustup/rustfmt 成功 exec 后旧兄弟线程仍存活并持有 stdout/stderr 管道写端，父 cargo 的 `ppoll` 永远收不到 EOF。内核现在线程组 leader 成功 exec 时同步终止并等待旧线程释放共享 mm/fd，内部 `_killed` 可打断 futex/pipe/nanosleep 等可中断等待，同时补齐线程组 wait/reparent 唤醒和并发 clone 屏障；未修改官方镜像或工具链绕过。
- [ ] 验证 `clone/clone3` 的 Rust/Cargo 实际 flags，包括 `CLONE_VM`、`CLONE_FS`、`CLONE_FILES`、`CLONE_SIGHAND`、`CLONE_SETTLS`、`CLONE_CHILD_CLEARTID` 和 `CLONE_PARENT_SETTID`。
- [ ] 验证线程 tid/tgid、TLS、clear_tid、线程退出、wait、robust futex、信号中断和父子回收。
- [ ] 验证 cargo build script、proc-macro、并发 rustc 子进程不会因 wakeup、管道、epoll 或 futex 丢失而挂死。
- [x] 按最终约束改为行为闭环：不注入 syscall 探针或诊断打印，只依据官方 BuildStorm 和定向 LTP 暴露的真实错误修复；未使用的 namespace flags 保持明确 ENOSYS。
- [x] 静态失败回滚已闭环待长压验收：legacy clone 按无符号 32 位解析实际 flags，未实现 namespace/pidfd/ptrace 明确拒绝；`CLONE_VM/FILES/SIGHAND` 失败只归还本次引用，不损坏父线程 mm/fd/sighand，所有失败 PCB 均撤销 RUNNABLE 并释放锁；clear_tid/robust futex 与线程组 exec/exit 继续由双架构定向测试和官方 Cargo 长压验证。

验收：glibc Rust 工具链可完成 `cargo new`、`cargo build` 和 Hello World，多次运行结果稳定。

### P0-D：QEMU 中观察 SMP 实际使用

- [x] 已收尾：删除无法在当前 `x86_64` 工作机完成 native KVM 验收的 schedbench 专项文件和计划项，不保留 `scripts/run/schedbench.sh`、`tools/smp/schedbench.c`。
- [x] 2026-07-26 初步观察：RV QEMU TCG `-smp 8` 下 OpenSBI 报告 8 个 HART，guest 内 `uname -m=riscv64`、`nproc=8`；这只说明 guest CPU 视图已看到 8 核，不构成吞吐性能证明。
- [x] 既有 QEMU TCG 8 vCPU 证据已确认 `/proc/cpuinfo`、`/sys/devices/system/cpu/online`、affinity 与 `getcpu()` 使用同一 online mask。
- [x] 既有 `f7ly_smp_cpu_bench`/stress-ng 结果确认 worker 实际分布到多个 guest CPU；仅作为多核使用证据，不写成 TCG 原生吞吐。
- [x] 单核观测问题未触发；当前 scheduler 保留 sticky home CPU，并用每 CPU 原子 runnable/running 压力做 O(NUMCPU) 初始选核。

### P1-A：补强 VM、缺页、COW 和回收

- [x] 代码完成待验收：同一 `CLONE_VM` 地址空间的 `mmap`、`mprotect` 和用户缺页路径已统一进入可睡眠内存锁；内存锁支持同线程重入，避免 copyin/copyout 懒补页重入死锁。
- [x] 代码完成待验收：匿名私有 mmap 合并后会重建 Maple Tree 索引，避免扩展尾部无法被后续 `mprotect`/缺页查到。
- [x] 代码与定向回归完成待验收：`brk` 使用历史堆高水位区分 MAP_FIXED 堆洞与新 mmap；允许 mmapstress03 要求的堆洞重增长，同时禁止越过高水位外的动态库/文件映射。
- [x] 双架构定向子集完成待长压验收：匿名/文件 mmap、brk/munmap 堆洞、mmapstress03/04/05、SysV SHM 与 COW/TLB 既有专项均通过；完整 BuildStorm 继续覆盖 mprotect/mremap/madvise 组合长压。
- [ ] 在大内存编译压力下增加或验证干净文件页回收、分配失败重试和 cache 驱逐，防止 rustc 因 page cache 吞噬可用内存而失败。
- [x] 代码审计与专项完成待 BuildStorm 长压验收：动态 VMASpace 承担 Rust 映射主路径，双架构 mmapstress03/04/05、COW 引用计数和跨核 TLB 专项通过；固定 legacy VMA 镜像不作为 256 项映射上限。
- [ ] 对 1G/4G/8G、单进程/多线程/多进程分别记录峰值内存和失败位置。

验收：完整构建期间无 OOM panic、页表损坏、COW 数据串扰、用户页权限错误或长期回收抖动。

### P1-B：补强 ext4/bcache 并发稳定性

- [x] 代码完成待验收：lwext4 挂载锁已替换为线程 PCB 所有权的可重入 FIFO 睡眠锁，且 mount lock 先安装、write-back cache 后开启。
- [x] 代码完成待验收：新增 `Ext4MountGuard`，已覆盖 `fstat/statx/truncate/utimens/ioctl inode flags/normal_file` 等直接调用 `ext4_fs_get_inode_ref()` 的 VFS 低层路径。
- [x] 代码完成待验收：bcache dirty list 改为双向 TAILQ，插入/删除为 O(1)，重复释放、活引用回收、链表指针损坏改为现场断言，不再吞掉 dirty-list 或引用计数错误。
- [x] 代码完成待验收：virtio-blk 容量改为读取设备真实 capacity，不再固定 4 GiB；`getdents64` 使用文件系统目录 cookie 更新 `_file_ptr`；`O_TMPFILE` 后备隐藏目录项在最后关闭时删除。
- [x] 2026-07-29 完成待验收：LA 官方 CAgent 的文件创建、读写、目录和搜索 4 项均通过；未观察到目录遍历永久阻塞、文件偏移异常或缓存不可见。
- [ ] 用并发 Cargo 构建验证 inode、目录、extent、journal、dirty buffer、LRU、flush/writeback 和 block device 的锁顺序。
- [x] 代码与定向回归完成待长压验收：RV `find` 路径缓存失效、LA bcache 树/热点悬空风险和 VFS 路径 ENOTDIR 优先级已修正；双架构路径/目录回归无损坏或卡死。
- [x] 定向回归完成待长压验收：`pread/pwrite/ftruncate/fallocate/fsync/rename/link/unlink/getdents64` 双架构通过；`fdatasync` 与并发 close 的最终压力仍由完整 BuildStorm 覆盖。
- [x] 已实现并通过定向回归：`statfs/fstatfs` 从实际挂载文件系统读取 blocks/free/available/inode/fsid/flags，双架构 `statfs02/fstatfs02` 无失败。

验收：CAgent 文件项双架构通过；BuildStorm 运行期间无文件系统损坏、目录遍历永久阻塞、dirty cache 丢失或 journal 错误。

### P1-C：统一 Linux 信息、时间和网络状态语义

- [x] 代码完成待验收：`/proc/uptime` 和 `sysinfo.uptime` 已来自 `CLOCK_BOOTTIME`。
- [x] 代码完成待官方视图验收：`sysinfo` 进程数来自活动任务，1/5/15 分钟负载按 5 秒采样并导出 Linux 16 位定点，内存来自动态 PMM且 `mem_unit=PGSIZE`。
- [x] 定向回归完成待 BuildStorm 计时验收：clock_adjtime/settime、gettimeofday、futex timeout 与 epoll 零超时双架构通过；uptime 未缩放或伪造。
- [x] 已完成待验收：修正 `/proc/cpuinfo`、`/proc/self/status`、`/sys/devices/system/cpu`、affinity 和 `getcpu`，消除 CPU 视图的单核硬编码。
- [x] 代码完成待验收：`uname.machine` 按编译架构返回 `riscv64`/`loongarch64`。
- [x] 已完成待连续轮次验收：`/proc/version` 与 uname release 统一为 `6.17.0`，RV/LA 官方 CAgent kernel 单轮通过。
- [x] 已完成待连续轮次验收：补齐真实 `/proc/net/tcp{,6}` socket 快照；登记生命周期无悬空引用且不再因固定表耗尽 panic，RV/LA 官方 CAgent network 单轮通过。

验收：CAgent 的 date、cpu、kernel、network、fs-usage 均不依赖固定伪造值，且不会破坏 BuildStorm 的真实 guest 计时。

### P2：BuildStorm 性能优化

- [ ] 先取得内核能跑通的成功基线，再拆分 syscall、调度、缺页、文件缓存、ext4 元数据和 block I/O 耗时。
- [x] 代码优化完成待完整构建验收：初始选核退化为 O(NUMCPU) 原子压力读取，调度扫描先锁 PCB 消除跨核竞态，唤醒使用语义化 SMP IPI；路径组件缓存、bcache 热点和 COW/TLB 批处理已做定向回归；epoll 零超时对任意事件上限使用 8 项栈缓冲与轮转扫描、不分配页、不睡眠，ET/LT 公平修复已在 RV/LA `epoll_wait04` 连续 10 次回归中通过。
- [x] 取舍已执行：未跳过 crate、未复用旧 target、未减少 QEMU 核数，也未修改官方脚本或 uptime。运行期任务迁移仍不启用；2026-08-09 针对 BuildStorm 停顿证据重新加入按 CPU 的 runnable 位图，仅改变调度候选扫描，不改变任务迁移契约，并已通过双架构强制构建和短测。
- [x] 2026-07-29 冷构建性能基线完成待验收：同一 RV 8G/8 vCPU 镜像从空 `target/debug` 运行 900 guest 秒，旧内核未输出 `Compiling`，仅留下 13 个 deps 文件；宿主采样显示 8 个 vCPU 线程长期合计约 800% CPU，主要消耗在空闲调度扫描。
- [x] 2026-07-29 多维热路径优化完成待验收：scheduler 使用活跃 PCB 位图、默认优先级无锁快路和 idle/IPI 唤醒；futex/普通 wakeup 遍历活跃槽位；ext4 FIFO 锁精确唤醒、块缓存扩为 8192 项并使用描述符池；COW/页表清零和用户 IO 缓冲删除重复写；匿名私有 `MADV_DONTNEED` 实际释放驻留页。
- [x] 2026-07-29 Cargo 深路径优化完成待验收：root 打开文件不再对每级前缀重复 resolve+stat，存在性和文件类型查询合并为一次。相同 RV 冷诊断从约 415 guest 秒才启动并行 rustc、535 秒 5 个 deps 文件，提升到 107 秒启动并行 rustc、482 秒一度达到 20 个 deps 文件、534 秒仍有 15 个；8 个 rustc 已实际分布到 CPU0-7。
- [x] 2026-07-29 第二阶段调度/ext4 优化完成待验收：新任务按各 home CPU 活跃压力选择最空闲核，避免短命 shell 扰乱轮转相位并让重型 rustc 与 cargo 撞核；ext4 增加 16 槽元数据热点直达缓存，同时修正区间失效起点不存在时漏失效的问题。相同 RV 冷构建在约 107 秒进入 rustc，热点缓存于 149/191/234/277 秒达到 4/6/7/10 个 deps 产物，无热点对照同期仅为 1/3/3/4；运行期任务迁移因会破坏 guest sleep/计时稳定性，已撤回。
- [x] 2026-07-31 第三阶段深路径优化完成待验收：lwext4 增加由目录项权威增删路径维护的正/负组件缓存，symlink 解析改为无临时组件 vector 的单遍扫描，普通读采用已通过时间语义窄测的 relatime 以减少重复 atime 写放大。相同 RV 冷构建在 74.54 秒采样时首批 rustc 已运行约 20～23 秒（旧内核 106.99 秒采样时运行约 27 秒，推算启动约从 80 秒提前到 51 秒）；目录项缓存轮次于 115.66/238.26/282.84/335.91 秒达到 5/10/12/14 个 deps 产物，旧热点缓存轮次于 106.99/234.26/277.26 秒为 0/7/10 个，旧同长度轮次 345.57 秒为 11 个。RV/LA 路径负转正、rename/unlink、目录 rename、symlink 类型切换窄测均通过，未见 panic。
- [x] 2026-08-01 原 `Compiling core` futex 卡死修复完成待验收：以修复前/后的 8 vCPU GDB 全核栈确认并消除 futex 全局锁与 PCB 锁的 ABBA；WAIT/WAKE 使用同一规范锁序，futex key 改为原子预筛选发布。RV/LA 四个 futex 语义小测结果一致，未跳过 crate、未降低 QEMU 核数，也未按测例增加特判。
- [x] 2026-08-09 BuildStorm 停顿诊断与代码修复完成待完整验收：已完成当前 HEAD 8/1 vCPU 与 `a40074f6^` 独立 worktree 的有界对照、间隔 GDB/计数器快照；按证据完成地址空间定向 TLB、ext4/bcache、futex/普通 wait channel、调度候选扫描、特殊映射和退出回收链路修复，并保留默认关闭的诊断框架与宿主探针。
- [x] 2026-08-09 修复后受控结果：官方进度探针在 `logs/run/output_r_20260809-073601_buildstorm-perf.txt` 中进入 `core v0.0.0` 并生成产物，但在 1200 秒窗口内仍未出现后续 `Compiling`；因此“根因链代码修复”与“完整 BuildStorm 成功”分开记录，后者继续待用户验收。
- [x] 2026-08-11 第一阶段性能优化已完成待验收：地址空间批量 teardown、文件页缓存、复用 syscall 缓冲、默认关闭的性能计数与正式探针、内核无浮点 ABI/指令门禁均已收口；后续短跑暴露并修复了 freestanding 内核未执行文件页缓存全局构造器、以及 exec 换映像时旧 mm 最终清理被误当作非法退出两个 panic。RV/LA evaluation 构建和无浮点门禁通过；RV 3 分钟定向短验证中 CAgent 10/10 完成，BuildStorm 通过 toolchain/minibuild/预热并进入正式并行 crate 编译，无 panic/assert/kerneltrap。本轮未运行 Docker 或完整 BuildStorm，完整正确性与性能数据仍待后续人工长验收。
- [x] 2026-08-11 可复用性能观测框架完成待验收：`PERF_DIAG=1` 提供统一指标表、per-CPU epoch 计数、syscall 耗时、timer/PMU 热点与可选调用链、`/proc/f7ly/perf` v1 TSV ABI、双架构静态 `f7ly-perf` 和两阶段内核符号表；普通 RV/LA 内核已确认无相关符号及 ABI 字符串。双架构短 smoke 覆盖 status、stat、timer 平面/调用链、auto 回退、显式 PMU 不支持、reset/epoch 和非法命令，最终日志为 `logs/run/output_r_20260811-205033_buildstorm-perf.txt` 与 `logs/run/output_l_20260811-205133_buildstorm-perf.txt`；未运行完整 BuildStorm 或 Docker autotest。
- [x] 2026-08-11 clone 缺页锁生命周期修复完成待长验收：GDB 证实 `CLONE_CHILD_SETTID` 在持有未发布子 PCB 自旋锁时触发 `copy_out -> fault_page -> memory_lock sleep`，导致 scheduler 的 `num_off==1` 不变量失败。现子任务保持 `USED`，`CHILD/PARENT_SETTID` 用户写入在不持子 PCB 锁时完成，再一次性发布 `RUNNABLE`；创建失败回滚也不再带非当前 PCB 锁进入可睡眠资源清理。RV 5 分钟原路径复跑越过 `libc v0.2.186`，继续编译到 `ax-posix-api`，无 panic/assert/kerneltrap；未替代完整 BuildStorm 长验收。
- [x] 2026-08-11 mm 最后引用并发归还修复完成待长验收：`free_all_memory()` 的本次原子 `fetch_sub` 结果现在唯一决定最终清理与 `delete` 所有权，PCB 在归还前先摘掉 mm 指针，创建失败和 exec 回滚也使用同一契约，消除非最后线程误删正在 teardown 的 mm 及持有者误计泄漏窗口。RV 24 轮×8 线程原始 `SYS_exit` 竞态专项通过，无 panic/引用漂移/最终销毁断言；RV/LA evaluation 构建与无浮点门禁通过，未运行完整 BuildStorm。
- [x] 2026-08-12 LA 完整 BuildStorm 单轮通过待重复验收：正式路径从 toolchain/minibuild 运行至并行 Cargo 完成，输出文件 1714568 字节，guest 耗时 1425.69 秒；完整日志无 rustc ICE、panic 或 shootdown timeout。原始决赛镜像 `e2fsck -fn` 返回 0；探针临时镜像的 inode 558/560 提示来自 `debugfs unlink + write` 替换两份 runner 的注入副作用，不是 guest 文件系统损坏。
- [ ] 待完成：在不受其它 QEMU 抢占的环境中让 RV/LA 各取得至少两次完整 BuildStorm 重复数据；当前 RV 用户基线日志和 LA 修复后日志各有一轮完整 PASS，尚不把单轮结果记为双架构重复长测全部验收。

## 6. 内核能力验收门槛

### CAgent 内核门槛

- [x] 连续轮次门槛完成待验收：RV64/glibc 和 LA64/glibc 官方原始 CAgent 各连续三轮取得 10/10，六轮均正常抵达组结束标记；
- [x] 2026-07-29 LA 功能链路完成待验收：最终候选按 `cagent_test(); buildstorm_test();` 顺序运行，CAgent 复跑 10/10 通过；另一次并发 LLM 运行中 `fs-readwrite` 单项 reject、其余 9 项通过，未复现为持续文件系统错误。
- [x] 代码与定向回归完成待连续官方验收：`statfs`、CPU 数量、kernel version、时间和 TCP 状态均来自真实内核状态；
- [x] 双架构定向回归完成待连续官方验收：文件创建、读写、目录遍历、rename/link/unlink/getdents 在关闭文件后保持一致；
- [x] 双架构 78 次定向回归日志无 syscall panic、长期卡死、TLB timeout、TFAIL 或 TBROK；官方 CAgent 双架构连续三轮门槛已完成待验收。

### BuildStorm 内核门槛

- [x] `-smp 8 -m 8G` 稳定启动，8 个 CPU 在 guest CPU 视图中一致；
- [x] PMM、buddy、refcount、页表和 COW/TLB 专项覆盖 8G，不使用 1GiB 固定上限；
- [x] 2026-07-29 LA 最小实编译与官方正常启动完成待验收；完整 `cargo xtask arceos build` 仍按下一项单独验收。
- [x] 2026-07-29 RV/LA 官方 BuildStorm 前段完成待验收：两架构均通过 toolchain 与 minibuild，从 `Resolving dependency graph...` 继续真实并行启动 `unicode-ident`、`quote`、`proc-macro2`、`libc`、`cfg-if`、`log`、`find-msvc-tools` 等 crate；LA 同轮先完成 CAgent 10/10，日志中未见 panic、锁断言或 shootdown timeout。
- [x] 2026-08-01 已修复并短验收 `Compiling core` 暴露的 futex/PCB 锁序死锁；双架构四个 futex 语义用例通过，修复后 GDB 不再存在原两 CPU 闭环。
- [x] cargo build script、proc-macro、多线程 futex/信号/等待链路已由 RV/LA 各一轮完整构建覆盖；
- [x] 完整 `cargo xtask arceos build` 已在 RV/LA 从零完成，产物分别为 1681000 和 1714568 字节；
- [x] 两轮完整构建无内存 panic、文件系统损坏、死锁、丢唤醒、页表错误或设备中断错误；
- [x] `/proc/uptime` 和 `cores` 使用真实内核状态，正式结果均记录 `cores=8` 和 guest elapsed；重复轮次仍由前述独立开放项约束。

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
- `scripts/run/smp_cpu_bench.sh`、`tools/smp/f7ly_smp_cpu_bench.c`：原生 KVM 环境下的双架构 CPU 压测；当前 x86_64 工作机仅用于 QEMU TCG 多核使用观察，不用于性能结论；
- `kernel/proc/proc_manager.cc:2233-2655`、`kernel/proc/futex.cc:82-494`：已有 clone/线程/futex 基础；
- `kernel/mem/virtual_memory_manager.cc:1342-1733`：已有 COW 处理；
- `kernel/sys/syscall_handler.cc`、`kernel/fs/vfs/vfs_utils.cc`、`kernel/fs/lwext4/ext4.cc`：statfs/fstatfs 已使用真实 ext4 mount stats；sysinfo 使用真实 uptime、活动任务数、动态 PMM 与 5 秒负载采样；
- `kernel/fs/vfs/file/virtual_file.cc`：按 online CPU 集合生成 proc/sys CPU 视图；
- `kernel/fs/vfs/vfs_ext4_ext.cc:34`、`kernel/fs/vfs/vfs_utils.cc:4849`：ext4/find/bcache 和锁风险注释；
- `kernel/fs/lwext4/ext4_bcache.cc:89-307`、`kernel/fs/lwext4/ext4_blockdev.cc:169-242,407-430`：当前 buffer cache/LRU/写回路径。

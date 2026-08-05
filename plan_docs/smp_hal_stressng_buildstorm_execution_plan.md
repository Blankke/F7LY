# SMP HAL、stress-ng 与 BuildStorm 自编译执行计划

状态：二阶段代码已收尾；SMP、真实 stress-ng、8 GiB PMM 和双架构 TLB 已完成 QEMU 验收；旧 selfhost 制备链路已由预编译 tg-xtask 的官方 BuildStorm 镜像替代，官方长测仍待验收

日期：2026-07-18

## 1. 目标与边界

本计划承接 `final_2026_support_gap_plan.md` 中已经完成待验收的 SMP 8 基础能力，分两阶段完成以下工作：

1. 整理当前 SMP 实现的架构边界，把 CPU 拓扑、次核入口、次核本地初始化和启动协议从 `main()` 移入 HAL；随后使用用户已放入 RISC-V shell rootfs 的真实 `stress-ng` 验证多核并行利用率。
2. 面向 BuildStorm 补齐 8 GiB 物理内存管理、通用跨 CPU TLB shootdown，并复用 StarryOS 的 Debian selfhost rootfs 制备思路，在 F7LY 内执行真实离线 Cargo/ext4 长压力。

本轮不通过以下方式制造“成功”结果：

- 不把仅完成 RV/LA 编译当作 SMP 或 8 GiB 运行正确；
- 不以自制 pthread 素数程序替代已经存在的真实 `stress-ng`；
- 不复用 guest 内已有 `target/` 目录冒充从零编译；
- 不修改 `/proc/uptime`、结果标记或产物大小绕过验证；
- 不直接写回 `images/` 下的原始 rootfs，自编译和持久写测试一律使用 `build/` 下的工作副本。

## 2. 已确认的基线与参考

### 2.1 F7LY 当前基线

- SMP 基础实现位于提交 `d0bd378`，支持 RV64/LA64 的 1/2/4/8 CPU 启动和调度。
- 改造前两个 `kernel/boot/<arch>/main.cc` 曾包含 CPU 拓扑发现、次核启动、次核入口和停核逻辑；当前已迁移到 `kernel/hal/<arch>/smp.cc`，这里只保留该历史基线说明。
- `Cpu` 已提供 possible/online mask、bootstrap gate、scheduler gate 和每核 current process，可作为 HAL 的通用状态源。
- `images/rootfs-riscv64.img` 已包含真实 `/usr/bin/stress-ng` 及 `libJudy`、`libsctp`、`libbsd`、`libz` 等动态依赖。
- 改造前 RISC-V 的 `PHYSTOP`、固定 262144 项页引用计数表和固定 320 页 buddy metadata 共同阻塞 8 GiB；当前已改为 DTB 驱动的动态 PMM metadata，并通过 `-m 8G -smp 8` QEMU 验收。
- 改造前 RISC-V 的 PTE/页表指针检查以静态 `PHYSTOP` 为上界；当前已切换到 PMM 实际 managed range，并通过 8G TLB/COW 专项验收。
- 当前 RV 使用同步 SBI remote `sfence.vma`，LA 使用 IOCSR TLB IPI 的 generation request/ack 协议；两架构跨 CPU 用户态专项均已通过。

### 2.2 StarryOS 参考点

- `922bedc18` 的 run queue 改动优先占用 idle CPU，再按 runnable load 选择 CPU；F7LY 已有初始轮转，本轮只借鉴其“用真实吞吐判断 CPU 是否被利用”的验证方法，不直接移植 Rust 调度器。
- `scripts/sysbench-cpu.sh` 使用预热、重复运行、结构化结果行、吞吐和加速比汇总；本轮的 `stress-ng` runner 沿用这一测量框架。
- Starry selfhost 已证明自编译会触发 FDT 内存容量、页分配元数据、信号唤醒、文件缓存回收、ext4 和 Cargo 子进程等系统级问题。
- `scripts/prepare-selfhost-rootfs.sh` 及 selfhost guest 脚本可提供 Debian/Rust/Cargo/offline registry 的兼容镜像，不重复发明工具链安装流程。

## 3. 阶段一：SMP HAL 重构与真实 stress-ng

### 3.1 HAL 分层设计

新增通用入口 `kernel/hal/smp.hh`，由当前架构目录各自实现。boot `main()` 只保留全局子系统初始化顺序，SMP 生命周期由 HAL 封装。

计划 API：

```cpp
namespace hal::smp {
// 在 main() 最早阶段调用。非主核在内部进入 secondary 路径且不返回；
// 主核完成 CPU 槽位认领、DTB 初始化前置条件后返回。
void enter(uint64 cpu_id, uint64 boot_argument);

// DTB 可访问后发现 possible CPU，并发布统一拓扑。
void configure_topology();

// 全局对象和主核本地中断均就绪后启动次核，等待 online，并放行 scheduler。
void start_secondaries(uint64 boot_argument);

[[noreturn]] void park_current_cpu();
}
```

职责划分：

- `kernel/boot/<arch>/main.cc`
  - 只负责一次性的 Printer、trap 全局对象、PMM/VMM、进程、设备、VFS 和 scheduler 初始化；
  - 不包含 HSM、IOCSR mailbox、secondary local CSR、possible/online 等细节。
- `kernel/hal/riscv/smp.cc`
  - OpenSBI HSM 启动；
  - secondary hart 等待 bootstrap gate；
  - 激活本核 SATP、trap、PLIC、online 状态和 scheduler gate；
  - 非法/未声明 hart 停核。
- `kernel/hal/loongarch/smp.cc`
  - 保留并封装 IOCSR mailbox/boot IPI；
  - secondary CPU 的 PGDL/PGDH、trap/timer、online 和 scheduler gate；
  - 运行期 IPI 的本地初始化接口为阶段二预留清晰位置。
- `kernel/hal/cpu.*`
  - 继续作为架构无关 possible/online/bootstrap/scheduler 状态机；
  - 不把设备寄存器、SBI 或页表安装细节放入 `Cpu`。

重构不改变的启动不变量：

1. PMM/VMM/VFS/设备/进程池只初始化一次；
2. 次核在全局对象完成前不能打开会访问全局状态的中断；
3. 每个 possible CPU 完成本地页表、trap 和中断初始化后才置 online；
4. 所有 possible CPU online 后才统一放行 scheduler；
5. RV/LA 主核启动先后差异封装在架构 HAL 内，不在公共主入口堆条件分支。

### 3.2 stress-ng runner 设计

新增 `scripts/run/smp_stress_ng.sh`，只使用 RISC-V `images/rootfs-riscv64.img` 的真实 `stress-ng`，默认参数：

- QEMU vCPU：8；
- worker 矩阵：1、2、4、8；
- 每组：一次预热、三次计量；
- 单次时长：10 秒，可通过参数缩短；
- CPU stressor：先以 stress-ng 默认 CPU method 建立可复现基线；
- QEMU 使用 `-snapshot`，不修改原始镜像；
- 每轮完整串口输出写入 `logs/run/`，汇总 TSV 同目录保存。

guest 输出使用唯一的开始/结束标记包围每一轮，host runner 从标记区间内提取
stress-ng 的 CPU 指标行并写入 `metrics.tsv`；这样不会把启动日志或 shell 回显
误判为计量结果。例如：

```text
__F7LY_STRESS_BEGIN__ workers=4 run=2
stress-ng: ... cpu ... bogo_ops ... bogo_ops_per_second ...
__F7LY_STRESS_END__ workers=4 run=2 rc=0
```

汇总指标：

- `throughput(N)`：同一 worker 数三轮 `bogo ops/s real time` 的平均值，同时保留 min/max；
- `speedup(N) = throughput(N) / throughput(1)`；
- `efficiency(N) = speedup(N) / N × 100%`；
- 记录 guest 可见 CPU 数、online mask 和每轮退出码。

验收分为正确性和性能趋势两层：

- 正确性硬门槛：1/2/4/8 worker 全部正常退出；无 panic、page fault、非法 CPU、死锁、stress-ng verify failure；guest 可见 8 CPU。
- 并行利用硬门槛：`throughput(4) > throughput(1)` 且 `throughput(8) > throughput(1)`。TCG、宿主过载和 stressor 差异会影响线性程度，因此不把固定 4×/8× 作为功能正确性的硬门槛。
- 趋势诊断：若吞吐不增，结合 scheduler 运行核分布、`getcpu`/affinity 结果和宿主 TCG 配置定位；不能只凭所有线程“创建成功”宣布多核有效。

### 3.3 阶段一执行顺序

- [x] 建立 `hal::smp` 通用接口和 RV/LA 实现。
- [x] 将两个 `main.cc` 中 SMP 私有逻辑移除，保持原初始化时序。
- [x] RV/LA evaluation 与 shell 两种模式编译。
- [x] RV/LA 各运行一次 8 CPU shell 启动烟测，确认全部 CPU online。
- [x] 运行 stress-ng 短矩阵，验证命令和结果解析。
- [x] 生成真实 stress-ng 吞吐、加速比和效率汇总。
- [x] 将结果和复现命令回填复现文档；不把 LA 缺少 stress-ng rootfs 伪装成 LA 性能实测，临时日志已在收尾阶段清理。

## 4. 阶段二：8 GiB、TLB shootdown 与 Cargo/ext4 长压力

### 4.1 8 GiB 物理内存模型

目标是让 PMM 由 DTB 实际 RAM 区间决定容量，并让分配器元数据随容量增长，而不是把常量改成 8 GiB。

#### DTB 区间选择

- RV：选择包含内核镜像的 RAM 区间；起点跳过 kernel 和动态 metadata，终点取 RAM top，并排除 DTB 所在尾部页。
- LA：保留包含内核的低端连续区用于 kernel linear map；若 QEMU 把内存拆成低端和 `0x90000000` 以上高端区，选择足够大的高端连续区承载 PMM/heap，所有地址统一转换为 DMWIN 内核可访问地址。
- 所有区间计算使用溢出检查和页对齐；MMIO、空洞、kernel、DTB、initrd 不能进入 buddy。

#### 动态 buddy 与 refcount metadata

- `BuddySystem` 增加按 `total_pages` 计算 tree 节点数和 metadata 字节数的接口。
- `Initialize()` 显式接收 tree storage 和长度，不再从 `base_ptr - BSSIZE*PGSIZE` 反推固定区。
- PMM 在可用区间前部迭代计算：`BuddySystem 对象 + tree + uint16 refcount[page_count]` 的总页数，直到 metadata 页数和最终可管理页数稳定。
- 页引用计数改为指向动态 metadata 的指针，删除 262144 页固定数组和 1 GiB panic。
- HMM 也已使用同一套动态 metadata 计算接口；PMM 和 HMM 均通过显式 storage 初始化，不再保留隐式 `BSSIZE` 契约。
- RISC-V PTE/页表合法性判断改用 PMM 动态实际区间，消除静态 `PHYSTOP` 对高端页表的误杀。

容量验收数据通过 guest 的 `sysinfo`/`/proc/meminfo` 读取并由专项程序输出结构化结果：

- DTB RAM 区间；
- PMM metadata 起止、tree 字节、refcount 字节；
- managed page 起止和页数；
- heap/shm 区间；
- 这些区间不得重叠。

### 4.2 通用跨 CPU TLB shootdown

新增架构无关接口 `kernel/hal/tlb_shootdown.hh`：

```cpp
namespace hal::tlb {
void flush_local_range(uint64 start, uint64 size);
void flush_range_all_cpus(uint64 start, uint64 size);
void flush_all_cpus();
}
```

首版以正确性优先：所有 online CPU 都参与失效，不依赖尚未完善的 ASID/地址空间驻留追踪。NCPU 只有 8，保守广播的成本可接受，后续再按 active page table mask 优化。

架构后端：

- RV：本核使用 `sfence.vma`；远端使用 OpenSBI remote `sfence.vma`，mask 仅包含 online 且非当前 hart。SBI 调用返回视为同步完成。
- LA：保留 boot IPI vector 0，新增运行期 TLB vector；每核维护 request/ack generation。发送方以 release 发布请求并发送 IOCSR IPI，接收方先 clear IPI status，再本地全 TLB 失效并以 release 回写 ack；发送方 acquire 等待全部目标 ack。
- 并发发送通过单调 generation 和“只推进、不回退”的 per-CPU request 合并，避免两个 CPU 同时 shootdown 时互相覆盖。
- CPU 只有在本地 IPI 已使能后才可置 online；上线边界执行一次全本地 TLB flush，消除“发送方刚好跳过 becoming-online CPU”的窗口。

接入顺序必须满足“先撤销 PTE，远端确认失效，再释放/复用物理页”：

- `vmunmap`；
- `mprotect/protectpages`；
- fork/COW 把父页降为只读；
- COW fault 替换物理页；
- signal stack/special mapping 的原地 PTE 修改；
- 其它会撤销权限、撤销映射或替换 PA 的路径。

单纯建立此前不存在的新映射可以不广播；但 remap/repair 路径必须显式声明是否可能覆盖旧翻译。

TLB 专项验收：两个线程固定在不同 CPU，共享同一 `CLONE_VM` 地址空间；CPU A 循环 `mmap/mprotect/munmap`，CPU B 持续读写并捕获预期信号。禁止出现撤销权限后仍成功访问、页释放后数据串扰、未知 page fault 或 shootdown timeout。RV/LA 均需运行。

### 4.3 selfhost rootfs 与脚本

新增目录 `scripts/selfbuild/`，包含：

- `prepare_rootfs.sh`：准备兼容的 RISC-V Debian selfhost 工作镜像；
- `guest_self_compile.sh`：guest 内执行的确定性离线构建脚本；
- `self_compile.sh`：构建 F7LY seed kernel、注入 guest 脚本、以 `-smp 8 -m 8G` 启动并验证结果；
- 必要的最小结果解析辅助脚本，不在仓库根目录生成临时文件。

rootfs 制备策略：

1. 优先复用 `~/tgoskits/tmp/axbuild/rootfs/rootfs-riscv64-debian-selfhost-v2.img`；不存在时，允许 `prepare_rootfs.sh` 调用 tgoskits 的 `scripts/prepare-selfhost-rootfs.sh --arch riscv64`。
2. Debian 基础镜像来源固定为 tgosimages v0.0.8 的 `rootfs-riscv64-debian.img.tar.xz`，校验清单中的 SHA-256；下载和工具链安装属于显式准备步骤。
3. tgoskits 源码默认由 `git archive HEAD` 注入 `/opt/starryos`，并记录 commit id；排除 `.git/`、`target/`、`tmp/`、日志和镜像，禁止把本地约 54 GiB 工作目录整体塞入 rootfs。
4. 把准备好的镜像稀疏复制到 `build/selfbuild/` 后再注入 F7LY 自有 guest 脚本；原始 tgoskits 镜像只读保留。
5. 制备前后运行 `e2fsck`，并通过 `debugfs stat` 验证 cargo、rustc、offline registry、`/opt/starryos/Cargo.toml` 和 guest 脚本。

guest 构建脚本要求：

- 输出源码 commit、CPU 数、内存、cargo/rustc 版本和磁盘可用空间；
- 删除或改用全新的 `CARGO_TARGET_DIR`，确保不是复用旧产物；
- `CARGO_BUILD_JOBS` 默认 8，可调低用于定位并发问题；
- 使用 `--offline`，避免把网络支持问题混入内核自编译能力；
- 每 30 秒输出心跳，区分“TCG 很慢”和“内核死锁”；
- 成功后把非空产物复制到 `/opt/f7ly-selfbuild-artifacts/`，打印唯一 `SELF_COMPILE_SUCCESS`；失败必须保留 cargo 退出码并打印 `SELF_COMPILE_FAILED`。

首个构建目标优先采用 tgoskits 已验证的 `cargo build -p starryos ... --offline`。成功后再运行更接近 BuildStorm 题目的 `cargo xtask arceos build` 或官方 runner；不能在第一条命令失败时直接跳到更大负载而丢失最小故障点。

### 4.4 Cargo/ext4 长压力分级

- L0：8 GiB、8 CPU 启动，读取 `/proc/cpuinfo`、内存和磁盘信息。
- L1：guest 内运行 `cargo --version`、`rustc --version`，创建/删除大量小文件，验证 ext4 持久写。
- L2：从空 target 构建一个最小 Rust crate，覆盖 rustc 子进程、pipe、futex、mmap 和链接器。
- L3：构建 StarryOS 单个核心 package，逐步把 jobs 从 1 提升到 2/4/8。
- L4：完整 `cargo build -p starryos --offline`，并验证产物。
- L5：运行官方 BuildStorm 等价命令和至少一次 30 分钟 ext4/进程长压力。

每一级失败都保留独立串口日志、最后一个 cargo crate、guest 心跳、PMM 统计和文件系统错误；修复后从该级复测，再继续升级。

## 5. 双架构验证矩阵

| 能力 | RV64 | LA64 | 验收方式 |
| --- | --- | --- | --- |
| HAL 重构后编译 | 必须 | 必须 | evaluation + shell build |
| 8 CPU 启动 | 必须 | 必须 | possible/online mask 全 8 位 |
| 真实 stress-ng 吞吐 | 必须 | 当前无对应包，不伪造 | RV rootfs 1/2/4/8 worker |
| 8 GiB 启动和 PMM 容量 | 必须 | 必须 | DTB/metadata/managed pages 日志 |
| 跨核 TLB 专项 | 必须 | 必须 | 固定两核共享地址空间压力 |
| selfhost Cargo | 主验收架构 | 编译和启动回归；有 LA 工具链镜像后补测 | RISC-V Debian selfhost |
| ext4 30 分钟压力 | 必须 | 必须 | shell 工作镜像、结构化结果标记 |

## 6. 实施与回退原则

- 每个阶段先完成双架构编译，再运行最窄烟测，再扩大压力；不在编译错误未清时启动长 QEMU。
- QEMU 串口输出始终写 `logs/run/output_*.txt`，聊天中只摘取结果标记和错误上下文。
- 原始 rootfs 只读保留；性能测试使用 `-snapshot`，selfhost 使用 `build/selfbuild/` 工作副本。
- 不删除用户已有文件或 tgoskits 既有产物；本轮创建且未完成的临时 selfhost rootfs、下载缓存、专项构建目录和 QEMU 日志可在收尾时清理。
- 若 selfhost 被未实现 syscall 阻塞，先记录真实 syscall/错误语义并补最小回归，不把 syscall stub 固定返回成功。
- 若 8 GiB 分配压力暴露文件页回收缺失，将回收作为独立后续能力实现；不能通过无限增大内存掩盖泄漏。

## 7. 完成定义

阶段一完成需要同时满足：

- [x] 两个 `main.cc` 不再承载次核生命周期复杂逻辑；
- [x] RV/LA 8 CPU 启动和现有 SMP 回归不退化；
- [x] 真实 stress-ng 1/2/4/8 worker 全部正常结束（RV rootfs）；
- [x] 结构化汇总显示 4/8 worker 相对 1 worker 有明确吞吐提升。

阶段二完成需要同时满足：

- [x] RV/LA 在 `-m 8G -smp 8` 下启动，PMM 实际管理容量不再受 1 GiB 固定表限制；
- [x] buddy tree、refcount 和页表检查全部使用动态容量；
- [x] RV/LA 跨核 TLB 专项通过，物理页只在远端确认失效后回收；
- [x] 已由预编译 `tg-xtask` 的新版官方镜像和镜像内 `/glibc/buildstorm_testcode.sh` 替代；不再制备或验收自有 `self_compile.sh` rootfs 链路；
- [x] 已由官方 BuildStorm 的空目标目录构建与产物检查替代；不再用自制 guest 离线构建作为决赛验收入口；
- [x] 已由官方 BuildStorm 4 小时上限长压替代；完整无 panic/死锁/页表或文件系统损坏证据仍在 `final_2026_support_gap_plan.md` 的官方两轮门槛下开放，不把替代写成测试通过；
- [x] 本计划的旧 selfhost 开放项已完成“验收链路替代”收口；实际官方命令、日志与未闭环项统一回填 `final_2026_support_gap_plan.md`。

## 8. 2026-07-18 实施记录

### 8.1 已完成代码

- SMP 生命周期已迁移到 `kernel/hal/smp.hh` 与两架构后端；boot `main()` 仅调用 `enter/configure_topology/start_secondaries`。
- buddy tree、PMM 页引用计数和 heap buddy metadata 已改为按实际页数计算，不再依赖固定 320 页和 262144 项表。
- PMM 按 DTB 选择连续 RAM；DTB/initrd 作为精确保留区切分 RAM，选取切分后的最大连续空闲段，不再因 blob 位于低端而丢弃其后的数 GiB RAM。RV 线性页表覆盖内核区和最终 allocator 所在完整 RAM，LA 保留合法的 64 位内存长度并可选择高端连续区。
- 新增通用 TLB shootdown；RV 使用同步 SBI remote sfence，LA 使用 IOCSR IPI 与 generation request/ack。
- `vmunmap`、COW、fork 降权、`mprotect`、`uvmclear` 和 signal stack 原地 PTE 修改已接入同步 shootdown；物理页释放排在远端确认之后。
- LA shootdown 等待和公共自旋锁加入无锁 TLB IPI 轮询，避免关中断时两个发送方互等，或目标 CPU 在等待发送方持有的锁时形成环形死锁。
- 新增真实 `stress-ng` runner，要求 guest 确认 CPU 数、所有计量轮次成功，并要求 4/8 worker 相对单 worker 有正加速。
- 保留并增强双架构静态 pthread CPU 基准：RV 使用 `make shell` 同源的 `rootfs-riscv64.img`，LA 使用现有评测盘；每轮固定使用同一组 8 vCPU，worker 逐核绑定并持续采样 `getcpu()`；guest 输出墙钟口径的 `events/sec`，runner 生成 `speedup` 和 `efficiency_percent`，避免把“worker 数变化”和“QEMU CPU 数变化”混成一个指标。
- 新增双架构 TLB 专项，固定 CPU0/CPU1，覆盖 `mprotect` 降权以及 `munmap + MAP_FIXED` 重映射两类旧翻译失效。
- shell initcode 依次支持 `/bin/busybox`、`/musl/busybox`、`/bin/bash` 和 `/bin/sh`，可直接进入 Alpine、评测 sdcard 与 Debian selfhost 三类 rootfs。
- 新增 `scripts/selfbuild/prepare_rootfs.sh`、`guest_self_compile.sh` 和 `self_compile.sh`，支持固定 SHA-256 的基础镜像、`git archive HEAD` 源码打包、L0-L5 分级、空 target、心跳、8 GiB/8 CPU 和非空产物/e2fsck 验收。guest 使用 `set -euo pipefail`，L5 默认执行至少 1800 秒空 target Cargo 与并发 ext4 churn，禁止 Cargo 失败后误报 PASS。

### 8.2 已通过验证

以下四个内核构建均成功：

```bash
make build ARCH=riscv INITCODE_MODE=evaluation
make build ARCH=loongarch INITCODE_MODE=evaluation
make build ARCH=riscv INITCODE_MODE=shell
make build ARCH=loongarch INITCODE_MODE=shell
```

以下静态专项和脚本检查均成功：

```bash
riscv64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
  tools/smp/f7ly_smp_cpu_bench.c -o build/smp-bench/f7ly_smp_cpu_bench-rv
loongarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
  tools/smp/f7ly_smp_cpu_bench.c -o build/smp-bench/f7ly_smp_cpu_bench-la
riscv64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
  tools/smp/f7ly_tlb_shootdown_test.c -o build/tlb-shootdown-test/f7ly_tlb_shootdown_test-rv
loongarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -pthread \
  tools/smp/f7ly_tlb_shootdown_test.c -o build/tlb-shootdown-test/f7ly_tlb_shootdown_test-la
bash -n scripts/run/smp_cpu_bench.sh scripts/run/smp_stress_ng.sh scripts/run/tlb_shootdown_test.sh \
  scripts/selfbuild/prepare_rootfs.sh scripts/selfbuild/guest_self_compile.sh \
  scripts/selfbuild/self_compile.sh
git diff --check
```

本轮还完成了 CPU 基准 runner 的静态指标闭环：结果目录包含 `metrics.tsv`（每个架构/worker 的 events/sec）和 `summary.tsv`（相对 worker=1 的 speedup、并行效率）。真实 QEMU 结果见 `docs/dev-notes/smp_buildstorm_reproduction.md`；临时日志已在收尾阶段清理。

### 8.3 尚未形成 PASS 证据的部分

- 双架构 CPU 基准、RV 真实 stress-ng、RV/LA 8 GiB PMM 与 TLB 专项均已形成 PASS 证据；证据数字和复现命令见复现文档。
- selfhost rootfs 制备已经启动并完成基础镜像下载、Rust 工具链安装和源码注入，但在 crates 预取阶段按收尾要求主动停止；没有把未完成的 Cargo/ext4 运行写成 PASS。
- `images/rootfs-loongarch64.img` 不存在，因此没有伪造 LoongArch 的真实 stress-ng 结果；LA 使用静态 CPU 基准完成多核吞吐和 affinity 验收。

### 8.4 恢复运行权限后的严格顺序

1. 复现已完成的验收：参见 `docs/dev-notes/smp_buildstorm_reproduction.md`。
2. 后续若继续 BuildStorm selfhost，再执行 `scripts/selfbuild/prepare_rootfs.sh --download-base --build-prepared`；该步骤需要网络、sudo、约 11 GiB 磁盘和较长的 Rust/crates 准备时间。
3. rootfs 成功制备后，依次执行 `scripts/selfbuild/self_compile.sh --level 0`、`--level 1`、`--level 2`、`--level 3 --l3-jobs 1`，再执行 L4 和 L5；任何一级失败都停在该级修复，不跳级。

## 9. 2026-08-02 BuildStorm 卡顿修复（完成待验收）

- [x] 修复动态匿名映射在页粒度 `mprotect` 后只拆分、不合并造成的 VMA 爆炸；仅在权限、标志、偏移和全部后端语义一致时合并，文件映射、共享映射、VmObject 与 overlay 均不参与。
- [x] 移除 `mprotect` 跨 VMA 更新的固定 16 段上限，改为完整记录权限并支持失败回滚，避免只改 PTE、不改 VMA 元数据。
- [x] 同一 BuildStorm `core` 编译进程的 VMA 数由 10026 降至 72；限时冷编译已从 `Compiling core` 继续产出 `libc` 与 `std` 编译产物。
- [x] 修复位于共享内存管理与 syscall 实现，不含架构分支；RV64、LA64 最终构建均作为短验收门槛，完整 BuildStorm 长测由用户执行。

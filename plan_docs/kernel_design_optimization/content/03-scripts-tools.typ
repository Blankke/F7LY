= 脚本、工具与性能诊断

== `scripts` 与 `tools` 的边界

`scripts/` 负责“怎样安全、重复地运行”：检查依赖、选择画像、构建内核、复制临时镜像、注入程序、启动 QEMU、设置超时、保存日志并判断结构化终态。`tools/` 负责“运行什么或怎样分析”：包含可交叉编译的 C/C++ 专项程序、`f7ly-perf`、LTP parser/ranker 和辅助修补工具。

这个边界使 guest 测试逻辑可审计，同时把容易误写原始镜像、混淆架构或遗失日志的宿主操作集中在脚本中。Agent 应优先调用脚本，而不是临时拼接一条不可追踪的 QEMU 命令。

== `scripts` 目录使用地图

#figure(
  table(
    columns: (3.2cm, 3.3cm, 1fr),
    table.header([*目录/脚本*], [*用途*], [*使用注意*]),
    [`scripts/images/`], [准备或恢复 QEMU 镜像], [`prepare-qemu-image.sh` 是普通运行入口；`restore-sdcards.sh` 会覆盖工作镜像，必须人工确认。],
    [`scripts/mount/`], [挂载 RV/LA 初赛或决赛 rootfs], [用于检查磁盘内容；挂载点和 loader 约定见开发调试文档。],
    [`scripts/run/`], [QEMU 回归、专项和性能探针], [默认使用临时副本或 snapshot；完整输出进入 `logs/run/`。],
    [`scripts/selfbuild/`], [准备 selfhost rootfs 和分级自编译], [需要较长时间、磁盘与外部依赖；不能替代官方 BuildStorm。],
    [`scripts/board/`], [2K1000 实板下板/启动辅助], [只在明确的实板任务中使用。],
    [`scripts/dev/`], [代码统计和 Git 烟测], [工程辅助，不作为内核功能证据。],
    [`generate_perf_` #linebreak() `symbols.sh`], [为诊断内核生成两阶段稳定符号表], [由构建系统自动调用，不应手工改写生成物。],
    [`check_kernel_` #linebreak() `no_fp.sh`], [检查 soft-float ABI、内核浮点/向量指令与 helper], [性能诊断代码也必须通过这一门禁。],
  ),
  caption: [`scripts` 使用地图],
)

常用窄验证入口包括：

```bash
scripts/run/futex_short_test.sh --arch all --qemu-cpus 8
scripts/run/tlb_shootdown_test.sh --arch all --rounds 200
scripts/run/ext4_concurrency_test.sh --arch all --qemu-cpus 8 --qemu-mem 8G
scripts/run/smp_stress_ng.sh --qemu-cpus 8 --worker-list 1,2,4,8 --runs 3
```

`smp_cpu_bench.sh` 的吞吐缩放结论要求对应架构的原生 KVM 宿主；x86_64 上的跨架构 TCG 只能观察 guest 多核行为，不能冒充原生硬件扩展性能。

== `tools` 目录使用地图

#figure(
  table(
    columns: (3.3cm, 3.2cm, 1fr),
    table.header([*路径*], [*内容*], [*与脚本的关系*]),
    [`tools/perf/`], [`f7ly_perf.cc` 与 native test], [编译为双架构静态 CLI，读取 `/proc/f7ly/perf`；native test 验证解析与聚合算法。],
    [`tools/smp/`], [CPU 吞吐、TLB shootdown、mm 释放竞态], [由对应 `scripts/run/` 脚本交叉编译并注入临时 rootfs。],
    [`tools/proc/`], [futex/线程退出短测], [由 `futex_short_test.sh` 编译和运行。],
    [`tools/fs/`], [ext4 并发一致性测试], [由 `ext4_concurrency_test.sh` 运行并在关机后执行只读 fsck。],
    [`tools/ltp/judge/`], [LTP 日志解析、排序、组合流水线], [用于批量结果分析，不直接修改内核 PASS。],
    [`tools/ltp/` #linebreak() `scoreboard/`], [历史 LTP 清单转换工具], [当前协作状态以顶层 `scoreboard/` 为准。],
    [`patch_loongarch_` #linebreak() `libctest_llsc.sh`], [LoongArch libctest LL/SC 辅助补丁], [只在对应用户态测试环境明确需要时使用。],
  ),
  caption: [`tools` 使用地图],
)

== `PERF_DIAG` 与 `f7ly-perf`

普通内核默认 `PERF_DIAG=0`，不承担采样代码和 frame pointer 开销。显式使用 `PERF_DIAG=1` 时，构建目录和内核产物增加 `-perf` 后缀，并启用统一指标表、per-CPU epoch 计数、syscall 耗时、timer/PMU 采样和可选调用链。

首先验证宿主侧解析算法和双架构 CLI：

```bash
make perf-native-tests
make perf-tools
```

然后运行不改变官方工作量的诊断 smoke：

```bash
PERF_DIAG=1 PROBE_MODE=perf-smoke \
  bash scripts/run/buildstorm_perf_probe.sh riscv

PERF_DIAG=1 PROBE_MODE=perf-smoke \
  bash scripts/run/buildstorm_perf_probe.sh loongarch
```

guest 中的主要命令为：

```bash
f7ly-perf status --json
f7ly-perf stat --interval-ms 1000 --count 3 --per-cpu --json
f7ly-perf top --backend auto --event cycles --frequency 100 \
  --callgraph --duration 10 --limit 20 --json
f7ly-perf reset all
```

`auto` 后端在 PMU 不可用时回退到 timer。显式 `pmu` 失败是能力报告，不应伪造为采样成功；timer 热点适合判断函数级分布，但 QEMU TCG 下的采样值不能外推到原生硬件绝对性能。

== BuildStorm 探针模式

`scripts/run/buildstorm_perf_probe.sh` 使用临时评测镜像，拒绝在已有 QEMU 竞争时启动，并将串口与宿主 vCPU 采样分别保存。`PROBE_MODE` 决定 guest 工作负载：

#figure(
  table(
    columns: (2.4cm, 3.2cm, 1fr),
    table.header([*模式*], [*用途*], [*适用阶段*]),
    [`progress`], [固定窗口观察 crate 和产物进展], [快速比较是否越过已知停顿点。],
    [`formal`], [空目标目录的正式冷构建和周期快照], [最终 A/B；计时轮应使用普通内核。],
    [`teardown`], [256 MiB 地址空间退出回收延迟], [定位 mm teardown。],
    [`filecache`], [同一大文件两轮并发遍历], [验证文件页复用和底层读取降幅，要求 `PERF_DIAG=1`。],
    [`qemu-ram`], [大范围预留、激活和触页], [定位大 VMA、回收和内层 QEMU ENOMEM。],
    [`free-count`], [高频空闲页统计], [验证 Buddy 空闲计数复杂度。],
    [`perf-smoke`], [`/proc/f7ly/perf` ABI 与采样测试], [诊断框架自身验收。],
    [`xtask`], [原始固定时长 BuildStorm 入口], [历史进度探针。],
  ),
  caption: [BuildStorm 性能探针模式],
)

正式诊断示例：

```bash
PERF_DIAG=1 PROBE_MODE=formal SKIP_CAGENT=1 \
  HOST_TIMEOUT=60m QEMU_MEM=8G QEMU_SMP=8 CHECK_E2FSCK=1 \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

最终计时必须再以 `PERF_DIAG=0`、同一镜像、同一 CPU/内存、空 target 和相同宿主负载重复运行。诊断数据用于找到瓶颈，普通内核时间才用于报告最终收益。

== 日志与实验身份

每轮 A/B 至少保存：Git commit 与 dirty 状态、完整命令、架构和 vCPU、QEMU/工具链版本、rootfs SHA-256、开始时间、退出码、结构化终态和原始日志路径。推荐在运行前记录：

```bash
git rev-parse HEAD
git status --short
qemu-system-riscv64 --version
riscv64-linux-gnu-g++ --version
sha256sum images/oscomp-final-riscv64.img
```

正式结果以唯一的 `BUILDSTORM_FORMAL_RESULT ok=true` 和 `BUILDSTORM_COMPILE ... elapsed_s=... cores=... bytes=...` 为准；仅有 QEMU 正常关机、出现若干产物或没有 panic 都不足以判定成功。

= 实验分析与性能结果

== 实验口径

正式 BuildStorm 使用 guest `/proc/uptime` 对 `cargo xtask arceos build` 计时，预构建 `tg-xtask` 不计入正式时间；正式目标架构的旧 target 在计时前删除。结果必须同时包含 `ok=true`、guest elapsed、vCPU 数和非空产物字节数。

诊断轮允许 `PERF_DIAG=1`、GDB 或宿主 CPU 采样；最终性能轮使用普通内核。A/B 必须保持 rootfs、Cargo 命令、vCPU、内存、QEMU 加速方式和宿主竞争状态一致。历史结果中没有完整保存这些字段的部分，在本章明确标注为限制。

== RV 并行编译启动阶段

2026-07-29 的同一 RV 8G/8 vCPU 冷诊断中，旧路径约 415 guest 秒才启动并行 rustc，优化后约 107 秒进入并行 rustc：

加速比：415 / 107 ≈ 3.88。

耗时降幅：(415 − 107) / 415 × 100% ≈ 74.2%。

这一数据反映“进入并行 rustc 的启动阶段”，不能当作完整 BuildStorm 编译加速比。优化后 8 个 rustc 曾实际分布到 CPU0—7，说明收益来自调度候选、初始选核和路径开销共同改善，而不是只提前打印日志。

== 双架构完整冷构建

2026-08-14 的历史正式探针记录如下：

#figure(
  table(
    columns: (1.7cm, 1.6cm, 2.4cm, 2.4cm, 1.9cm, 2.1cm),
    table.header([*架构*], [*vCPU*], [*优化前/s*], [*优化后/s*], [*加速比*], [*耗时降低*]),
    [RV64], [8], [3223.75], [1693.23], [1.904×], [47.5%],
    [LA64], [12], [2564.93], [1367.35], [1.876×], [46.7%],
  ),
  caption: [BuildStorm 完整冷构建历史 A/B 结果],
)

RV 产物为 1,681,000 字节，LA 产物为 1,714,568 字节；两轮均记录 `ok=true`，没有跳过 crate、复用旧 target 或修改 guest 计时。对应历史日志为：

- `logs/run/output_r_20260814-191332_buildstorm-perf.txt`；
- `logs/run/output_l_20260814-194725_buildstorm-perf.txt`。

这些日志位于本地 `logs/run/` 且默认不提交 Git。正式交付时应同时提供结构化摘要、日志 SHA-256 或单独证据包，避免评审者只能看到正文数字。

== Buddy 空闲页统计窄测

大内存回收路径曾在 `sysinfo` 中扫描 Buddy 状态计算空闲页。改为在分配/释放时 O(1) 记账后，LoongArch 在 32 MiB 碎片化、5000 次 `sysinfo` 条件下，单次耗时由 215,120 ns 降至 12,553 ns：

加速比：215120 / 12553 ≈ 17.1。

该窄测只证明空闲页统计复杂度下降，不应直接乘到 BuildStorm 总时间上。它与完整构建共同说明优化既处理可观测系统信息的热点，也没有破坏综合负载。

== SMP 与 TLB 辅助结果

既有复现记录中，RV 静态 CPU 基准的 1/2/4/8 worker 吞吐为 11524.665、24957.165、50653.834、99504.986 events/s，8 worker 相对 1 worker 为 8.634×；LA 对应为 16101.520、33044.438、66386.127、123023.106 events/s，8 worker 为 7.640×、效率 95.51%。

RV 真实 stress-ng 的 1/2/4/8 worker 吞吐为 35.270、69.190、142.120、271.240 bogo ops/s，8 worker 为 7.690×、效率 96.13%。这些结果用于证明 guest 能利用多个 CPU，不直接代表跨架构 TCG 的原生硬件绝对性能。

双架构 8 GiB TLB 专项覆盖 mprotect 降权、预期 fault、munmap 和 `MAP_FIXED` 重映射。历史 20 轮结果中，RV managed pages 为 1,751,684，LA 为 1,947,687，均完成 40 次预期 fault、20 次 mprotect 和 20 次 munmap。

== 正确性回归

性能结果之外，相关优化还使用以下门禁防止负收益：

- futex/线程退出短测覆盖 WAIT、WAKE、超时和 clear-tid；
- ext4 并发短测覆盖 `write/fsync/rename/read/unlink`，关机后对临时镜像执行只读 `e2fsck -fn`；
- 大 VMA 测试覆盖 8 GiB 预留、首尾触页、中段 mprotect 和完整 munmap；
- 大文件测试覆盖 3 GiB 文件偏移 mmap + fork；
- 双架构 CAgent、定向 LTP 和内核构建用于检查 Linux ABI 回归。

== 实验限制与复现改进

当前历史数据仍有以下限制：完整冷构建主要保留单轮结果；LA 使用 12 vCPU，不能与 8 vCPU 结果混成一张无条件横向对比；部分旧基线只在计划中记录，没有随 Git 提交原始日志和镜像哈希。因此本文给出的加速比可说明已观察到的收益，但最终评审复现应补充至少三轮同条件结果，并报告中位数、最小/最大值和异常轮原因。

推荐的结果表字段为：`arch`、`commit`、`dirty`、`profile`、`perf_diag`、`qemu_version`、`accel`、`vcpus`、`memory`、`image_sha256`、`elapsed_s`、`artifact_bytes`、`result`、`log_sha256`。这能把性能数字和可重放实验身份绑定起来。

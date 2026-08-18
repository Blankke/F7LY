= 附录

== 附录 A：最小复现命令

=== 构建与配置

```bash
make profiles
make print-config PROFILE=riscv-qemu MODE=evaluation
make build PROFILE=riscv-qemu
make build PROFILE=loongarch-qemu
make build PROFILE=riscv-visionfive2
make build PROFILE=loongarch-2k1000
```

=== 性能工具

```bash
make perf-native-tests
make perf-tools
PERF_DIAG=1 PROBE_MODE=perf-smoke \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

=== 定向回归

```bash
scripts/run/futex_short_test.sh --arch all --qemu-cpus 8
scripts/run/tlb_shootdown_test.sh --arch all --rounds 200
scripts/run/ext4_concurrency_test.sh --arch all --qemu-cpus 8 --qemu-mem 8G
scripts/run/smp_stress_ng.sh --qemu-cpus 8 --worker-list 1,2,4,8 --runs 3
```

=== BuildStorm 正式探针

```bash
PROBE_MODE=formal SKIP_CAGENT=1 HOST_TIMEOUT=60m \
  QEMU_MEM=8G QEMU_SMP=8 CHECK_E2FSCK=1 \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

完整 Docker 并发 workflow 不属于 Agent 默认执行范围。完成窄验证后由用户决定是否运行：

```bash
bash build/oscomp-eval-20260624-232828/scripts/run_docker_autotest.sh
```

== 附录 B：提交与设计索引

#figure(
  table(
    columns: (2.4cm, 3.5cm, 1fr),
    table.header([*提交*], [*主题*], [*本文位置*]),
    [`ad43daf7`], [调度、路径、ext4 热路径与并行 rustc 启动], [第 4—6 章],
    [`22f58137`], [SMP/futex/mm 并发与专项], [第 4、5 章],
    [`7d5a303c`], [BuildStorm 进程与 VFS 容量收口], [第 1、4 章],
    [`d092dec1`], [诊断、文件页缓存、批量回收], [第 3、5 章],
    [`28ea8f25`], [`/proc/f7ly/perf`、CLI、符号与采样], [第 3、5 章],
    [`dbec6fa5`], [内存、调度、ext4 性能收口], [第 4—6 章],
    [`1966b4ac`], [删除无效性能操作], [第 4 章],
    [`f4aa440b`], [大 VMA 与大文件偏移 64 位修复], [第 4、5 章],
  ),
  caption: [优化提交索引],
)

== 附录 C：评审证据清单

提交本文档时建议同时准备：

- 本文 PDF 与对应 Typst 源码；
- 当前内核 commit、dirty 状态和构建配置；
- A/B 每轮结构化 TSV/JSON，至少三轮；
- 原始日志或独立证据包及 SHA-256；
- rootfs 来源与 SHA-256、QEMU 和工具链版本；
- 关键提交 diff 或提交索引；
- AI 使用记录：提示词、模型、工具调用、人工接受/拒绝决定；
- 未完成项和外部验收边界。

== 附录 D：文档维护规则

本文源文件位于 `plan_docs/kernel_design_optimization/`，排版组件从 `docs/report-src/2026-final-final/` 导入。重新生成 PDF：

```bash
typst compile --root . \
  plan_docs/kernel_design_optimization/main.typ \
  plan_docs/kernel_design_optimization/f7ly-kernel-design-optimization.pdf
```

当性能数字、脚本参数、工具 ABI或最终报告结构变化时，应同步更新本文。若某项结果来源于历史日志而没有重新运行，必须继续标注“历史结果”，不能因为文档重新编译就改写为当前 HEAD 的新验收。

== 附录 E：当前结论

F7LY-OS 已经形成“文档路由—任务计划—脚本编排—专项工具—结构化日志—人工验收”的 AI 辅助内核开发流程。BuildStorm 优化展示了从系统现象到调度、锁序、地址空间、文件系统和大映射根因的逐层收口，也取得了明确的历史加速数据。

当前仍需补强的是性能结果的多轮统计和随提交交付的证据包。本文将这些限制保留为评审可见事实，并提供了补测与复现步骤，便于审核者在相同边界下重放开发过程。

已完成，待验收。

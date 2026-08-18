= AI 工具使用说明与开发复现

== AI 工具与责任边界

项目在 2026 年开发过程中使用 VSCode/Codex 交互环境中的 Codex，相关大模型为 GPT-5。AI Agent 用于阅读代码与 Git 历史、定位模块、生成候选补丁、构造窄测、调用脚本、分析日志、整理 scoreboard 和维护文档草稿。

本项目把 OpenAI 官方文档中“明确上下文、硬约束、成功标准和授权边界，并只暴露当前任务所需工具”的通用建议落实为仓库内契约：`AGENTS.md` 定义授权边界，`agent_docs/` 提供按任务加载的上下文，`plan_docs/` 保存阶段和证据，`scripts/` 与 `tools/` 提供可重复执行的受控入口。产品层原则可参见 #link("https://developers.openai.com/api/docs/guides/latest-model")[OpenAI 官方模型与工具调用指南]；审核时仍以本仓库中的命令、diff 和日志为准。

系统设计目标、比赛规则解释、是否接受方案、最终代码合入、测例是否计入通过、commit/push 和答辩材料由参赛队员负责。未经用户明确允许，Agent 不提交 commit、不推送、不重置历史；没有运行证据时不写 PASS；外部 Docker 完整 workflow 由用户决定何时执行。

== AI Agent 的输入与产出

#figure(
  table(
    columns: (2.2cm, 3.3cm, 1fr),
    table.header([*阶段*], [*Agent 输入*], [*可追踪产出*]),
    [建立上下文], [`AGENTS.md`、任务相关 `agent_docs`、Git 状态], [任务边界、当前架构事实和不变量。],
    [形成计划], [用户目标、现有 `plan_docs`、基线日志], [阶段、假设、风险和验收命令。],
    [定位问题], [scripts 输出、`f7ly-perf`、GDB、源码], [现象—证据—根因链，含被排除候选。],
    [候选实现], [权威对象、锁序、ABI 和架构边界], [可 review 的 diff、中文关键注释和回退点。],
    [验证], [静态门禁、窄测、A/B、完整负载], [日志路径、结构化终态、结果范围和开放项。],
    [人工验收], [diff、日志、正式报告], [接受/拒绝决定；必要时提交与 scoreboard 更新。],
  ),
  caption: [AI Agent 输入输出链],
)

== 可复用提示词模板

审核者可以在仓库根目录向 Codex 提交如下任务，以复现同类开发流程：

```text
请按 AGENTS.md 工作。阅读与 BuildStorm 性能相关的 agent_docs、
plan_docs/final_2026_support_gap_plan.md、近期 Git 提交和现有脚本。

目标：在不修改官方 Cargo 工作量、不复用旧 target、不修改 uptime 的条件下，
定位 <架构> BuildStorm 在 <阶段> 的瓶颈。

先记录 Git/宿主/QEMU/rootfs 身份，使用已有 buildstorm_perf_probe 和
f7ly-perf 做有界诊断；给出“现象—观测—根因”证据后再修改代码。
修改必须落在权威对象和通用不变量上，不写测例特判。

完成后依次运行：对应画像构建、无浮点门禁、最窄专项、同条件 A/B。
长日志写 logs/run，只汇报结构化终态。不要 commit、push 或运行完整 Docker
workflow；将结果和未完成项回填原计划，等待人工验收。
```

任务中的 `<架构>`、`<阶段>`、CPU/内存和最大运行时间必须明确。若没有足够信息，Agent 可以先做只读检查，但不能自行扩大到破坏性镜像操作或外部提交。

== 一次优化过程的复现步骤

以下步骤复现“BuildStorm 进度慢或停在 crate 编译”类任务；命令默认在仓库根目录执行。

=== 1. 固定仓库和环境

```bash
git status --short
git log -5 --pretty=fuller --stat
git rev-parse HEAD
qemu-system-riscv64 --version
sha256sum images/oscomp-final-riscv64.img
```

如果工作区已有用户改动，Agent 先读 diff，不能覆盖。基线与候选内核最好在独立 worktree 构建，并保留完整 commit；不要依赖模糊的“旧内核”文件名。

=== 2. 验证诊断框架

```bash
make perf-native-tests
PERF_DIAG=1 PROBE_MODE=perf-smoke \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

期望看到唯一 `PERF_SMOKE_RESULT ok=true`。若 PMU unavailable，`auto` 回退 timer 属于正常能力协商。

=== 3. 建立有界基线

```bash
PROBE_MODE=progress SKIP_CAGENT=1 HOST_TIMEOUT=10m \
  QEMU_MEM=8G QEMU_SMP=8 \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

记录最后 crate、产物数、rustc/cargo 状态、宿主 vCPU 分布和 panic/TLB timeout。若目标是完整时间，改用 `PROBE_MODE=formal` 并给足超时；progress 不能作为完整 PASS。

=== 4. 采样并形成根因证据

```bash
PERF_DIAG=1 PROBE_MODE=formal SKIP_CAGENT=1 HOST_TIMEOUT=20m \
  QEMU_MEM=8G QEMU_SMP=8 \
  bash scripts/run/buildstorm_perf_probe.sh riscv
```

结合 `/proc/f7ly/perf` 快照、宿主 CPU 日志和必要的 GDB 全核栈，比较 scheduler、futex、TLB、ext4 和 mm teardown。只有锁闭环、持续增长计数或同条件对照能支持根因结论；单个热点函数不能独立证明问题。

=== 5. 实现并运行窄验证

例如修改 futex 锁序后运行：

```bash
make build PROFILE=riscv-qemu
make build PROFILE=loongarch-qemu
scripts/run/futex_short_test.sh --arch all --qemu-cpus 8
```

修改 TLB/VMA 或 ext4 时分别改用 `tlb_shootdown_test.sh`、`qemu-ram`、`ext4_concurrency_test.sh`，同时保留双架构构建。

=== 6. 运行普通内核 A/B

诊断完成后以 `PERF_DIAG=0` 运行正式冷构建。基线和候选必须使用相同镜像、vCPU、内存和 Cargo 命令，各运行至少三轮；报告中使用中位数，并保留每轮数据。正式成功必须出现唯一 `BUILDSTORM_FORMAL_RESULT ok=true`。

=== 7. 更新计划并等待人工验收

Agent 将根因、实现、命令、日志、A/B 和限制回填原 `plan_docs`，标记“已完成，待验收”。只有人工确认 diff 和日志后，才决定是否提交以及是否更新正式报告或 scoreboard。

== AI 贡献披露样例

每个进入正式文档的 AI 辅助优化可以附一条记录：

#figure(
  table(
    columns: (3cm, 1fr),
    table.header([*字段*], [*记录内容*]),
    [日期与版本], [会话日期、Codex/GPT-5、仓库 commit、dirty 状态。],
    [原始任务], [用户目标与禁止事项，保留提示词或等价摘要。],
    [Agent 操作], [读取文档、调用脚本/工具、形成的候选根因和补丁文件。],
    [人工决策], [接受、拒绝或回退了哪些方案，理由是什么。],
    [验证证据], [构建、窄测、完整负载、日志和结构化结果。],
    [证据范围], [哪些已验证，哪些仍需重复长测或官方 harness。],
  ),
  caption: [AI 辅助优化披露字段],
)

这种记录让审核者能够沿“提示词—工具调用—代码 diff—日志—人工决策”重放开发过程，同时避免把 AI 生成候选方案误写成无人审核的最终设计。

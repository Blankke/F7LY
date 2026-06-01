# F7LY Agent 入口

本文档只保留所有 agent 必须先读的全局规则和导航信息。具体架构、开发调试、scoreboard 协作细节放在 `agent_docs/`，按任务需要再读取，避免每次启动都加载过多细节。

## 必读路由

所有 agent 在行动前必须先按任务类型读取下列文档。不要只读本文件就开始改代码：

| 任务类型 | 必读文档 | 用途 |
| --- | --- | --- |
| 理解架构、定位模块、判断改动边界 | [agent_docs/project_architecture.md](agent_docs/project_architecture.md) | 架构地图、模块职责、核心不变量、定位规则 |
| 构建、运行、调试、保存 QEMU 日志、挂载镜像 | [agent_docs/development_debugging.md](agent_docs/development_debugging.md) | 可直接复制的命令、日志规范、验证策略 |
| 查看/维护评测进度，更新 PASS 状态 | [agent_docs/scoreboard.md](agent_docs/scoreboard.md) 和 [scoreboard/README.md](scoreboard/README.md) | 四组合 scoreboard、状态列协作规则、生成器说明 |

如果任务同时涉及多个类型，就读取多个文档。scoreboard 已经是顶层目录，不在 `tools/` 下。

## 沟通与协作约定

- 默认使用中文回答。
- 非必要不使用花哨 Markdown；说明清楚、可执行、可追踪最重要。
- 修改代码前先看最近 Git 历史和当前工作区状态，避免覆盖人类或其他 AI 的改动。
- 不要在未明确要求时提交 commit、推送、重置历史；每一次 commit 都需要用户明确允许。
- 不要提交 `.env`。
- 不要随意删除或清空文件；除非用户明确要求，否则需要二次确认。
- 任务总结在聊天里完成，不要额外创建总结类 `.md` 文件。`README.md`、`AGENTS.md`、`agent_docs/`、scoreboard 等长期协作文档除外。
- 写入计划、To-do、任务汇报时使用中文，便于人类实时查看。
- 本项目是独立开发者的 OS Demo/评测项目，但实现仍优先选择清晰、可维护、可扩展的方案。
- “Demo 不考虑向后兼容”表示允许统一升级契约并删除旧入口；不要为了兼容旧字段、旧路由、旧函数名保留双套逻辑，除非用户明确要求短期过渡。
- 代码注释尽量使用中文说明关键流程和不变量；不要写空泛注释。
- 每次修改代码后主动验证语法、构建或相关测试；不能验证时要说明原因。
- 分析 jsonl/csv/xlsx 时只打印字段名、长度和前 N 字符，避免把长文本刷进上下文。

## Python 与环境约定

- 所有 Python 命令必须在项目对应 venv 中执行。
- 若没有 venv，使用 `uv venv` 创建；若默认 uv cache 不可写，可用 `UV_CACHE_DIR=/tmp/uv-cache uv venv`。
- 执行 Python 相关命令前必须激活 venv，并用 `which python` 或 `python -c 'import sys; print(sys.executable)'` 确认解释器路径。
- 开发或调试时如发现缺少环境，直接说明环境缺失，不要改代码做降级绕过。

## 项目定位

F7LY OS 是一个基于 xv6 思路扩展的教学/比赛用内核。主线目标已经从基本启动演进到支持 Linux ABI、BusyBox、glibc/musl 程序和 LTP 回归评测。

当前核心事实：

- 双架构：RISC-V 与 LoongArch。
- C++23 freestanding 内核，禁用异常和 RTTI。
- 面向 Linux ABI 的系统调用号和用户态调用约定。
- ext4 根文件系统为主，保留 FAT32 数据盘支持。
- 支持动态 ELF 装载、musl/glibc 动态链接器路径重写、shebang 脚本入口。
- 支持进程、线程、clone/clone3、futex、信号、mmap、共享内存、POSIX timer、epoll/eventfd/memfd 等大量 LTP 相关接口。
- 用户态 `initcode` 直接运行回归套件，然后调用 `shutdown()`。

## 文档路由

按任务类型读取对应文档：

- 架构定位、模块边界、核心设计思路：读 [agent_docs/project_architecture.md](agent_docs/project_architecture.md)。
- 构建、运行、日志保存、挂载镜像、单测调试：读 [agent_docs/development_debugging.md](agent_docs/development_debugging.md)。
- 查看评测进度、维护 Pass 状态、更新 scoreboard：读 [agent_docs/scoreboard.md](agent_docs/scoreboard.md) 和 [scoreboard/README.md](scoreboard/README.md)。
- 任务文档：读plan_docs目录下会包含当前或曾经我们做下的计划任务。每次完成的任务若来源于此，需要添加最简单的标记已完成待验收在文档相应位置。

## 常用入口速查

- 构建：`make build ARCH=riscv`、`make build ARCH=loongarch`
- 运行：`make run r`、`make run l`
- 用户态回归入口：`user/app/initcode-rv.cc`、`user/app/initcode-la.cc`
- 回归调度逻辑：`user/user_lib/user_test.cc`
- 系统调用定义：`kernel/sys/syscall_defs.hh`
- 系统调用实现与分发：`kernel/sys/syscall_handler.cc`
- LTP 参考源码：`ref/ltp/`或系统中`testsuits-for-oskernel/ltp-full-20240524/`目录
- scoreboard 入口：`scoreboard/README.md`

## Agent 工作流程建议

- 开始新任务时先读 `git status --short` 和最近 5 个 commit 摘要。
- 了解历史决策优先看 Git commit；必要时再看 README、`docs/dev-notes/` 和源码。
- 调试测例时先缩小到单条或小集合，不要一上来跑完整长回归。
- QEMU 长输出必须写入 `logs/run/output_*.txt` 日志文件，不要直接刷进聊天。
- 修改评测通过状态时，优先更新 scoreboard 对应小分文件的 `状态` 列，再重新生成顶层汇总。
- 维护 LTP 默认回归清单时，统一使用 `ltp_testcase` 表项中的四组合开关
  `{测例名, RV+musl, RV+glibc, LA+musl, LA+glibc}` 控制是否运行；不要在表外新增隐藏黑名单函数。
  若某组合暂不运行，直接把对应开关置为 `false`，并在该表项注释中写清原因和覆盖关系。

# 测例入口
sdcard下载地址为https://github.com/oscomp/testsuits-for-oskernel/releases/download/pre-20250615/sdcard-la.img.xz
若认为本地磁盘运行的行为与远端评测机不符，请重新下载并使用该镜像进行测试。注意解压后镜像文件较大，所以请不要经常做此操作，仓库中请只保留一份位于images文件下
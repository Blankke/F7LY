= 总结和展望

2026 年的 F7LY-OS 已经从“能够启动并跑通基础测例”的教学内核，推进到面向 Linux ABI、双架构连续测例和复杂用户程序的系统原型。结合当前 01 到 05 章的实现情况，内核已经把 CPU 拓扑、SMP 调度、内存屏障、等待唤醒、块设备、ext4、pipe、VMA 和网络链路组织成一套统一路径：RISC-V 与 LoongArch 共享同一套内核服务，用户态同时覆盖 musl 与 glibc 动态链接程序，根文件系统以 ext4 为主，网络链路、SMP 多核和页缓存路径也已经按 Linux 兼容语义打通。
== 已验证的功能

=== 连续测例通过情况

截至当前文档整理阶段，F7LY-OS 的连续测例覆盖已经从单点 syscall 验证，推进到按子系统和真实工作流组合验证。测例仍然按架构和 C 运行库分别计数，即同一个测例在 `RISC-V + musl`、`RISC-V + glibc`、`LoongArch + musl`、`LoongArch + glibc` 下会被视为不同验证项。

当前连续测例覆盖以下类型：

- *basic*：覆盖基础进程、文件、目录、时间、内存映射、管道、挂载与卸载接口。
- *busybox*：覆盖 BusyBox applet、shell 脚本、常用命令、文件浏览和命令行环境初始化。
- *cyclictest*：覆盖实时性和定时器延迟观测。
- *iozone*：覆盖顺序读写、随机读、反向读、跨步读、fwrite/fread、pwrite/pread、pwritev/preadv。
- *iperf*：覆盖 TCP/UDP 吞吐、socket 创建、连接、读写、关闭和脚本化网络测试。
- *libcbench*：覆盖 C 运行库性能路径、线程库、内存分配和常用 libc 接口。
- *libctest-static*：覆盖静态链接入口下的 libc 行为、pthread、TLS、数学函数、stdio、文件接口和进程接口。
- *libctest-dynamic*：覆盖动态链接入口下的 libc 行为、动态装载器、共享库解析、pthread、TLS、stdio、文件接口和进程接口。
- *lmbench*：覆盖系统调用延迟、上下文切换、管道、文件、内存、进程创建和通信微基准。
- *LTP*：覆盖 fork/exec/wait、mmap/munmap/mprotect、open/read/write/stat、signal、futex、timer、socket、epoll 等核心 Linux ABI 行为。
- *lua*：覆盖解释器启动、脚本执行、文件 I/O、随机数、字符串、排序和数学函数。
- *netperf*：覆盖 TCP_STREAM、TCP_RR、UDP_STREAM、UDP_RR、socket option 和网络吞吐/延迟脚本。

这些结果说明当前内核已经不再只验证单个 syscall 的存在，而是在动态链接器、文件系统、地址空间、线程同步、信号恢复、网络链路和进程生命周期之间进行组合压力测试。换句话说，当前的重点已经从“能不能启动”转成“能不能稳定承载真实 Linux 用户态工作负载”。

=== 用户程序运行能力

除自动连续测例外，F7LY-OS 现在已经把 shell、gcc、rustc、vim 和 git 这几类代表性复杂用户程序纳入同一套运行框架。这些程序覆盖的不是单一接口，而是现代 Unix 用户态环境的典型组合需求：命令解析与进程派生、阶段化编译、终端控制、多线程同步、内存映射、文件系统元数据、仓库对象管理以及 socket 网络通信。

#figure(
  text(size: 8pt)[
    #set par(justify: false)
    #show raw.where(block: false): box.with(
      fill: rgb("#fafafa"),
      inset: (x: 2pt, y: 0pt),
      outset: (y: 1pt),
      radius: 2pt,
    )
    #show raw.where(block: false): text.with(size: 6.8pt)
    #table(
      columns: (1.2cm, 2.5cm, 1fr),
      align: (center, left, left),
      inset: (x: 3pt, y: 2pt),
      table.header([*程序*], [*特征*], [*关键依赖与说明*]),

      table.cell(rowspan: 4, align: horizon + center)[#text(size: 9.2pt)[*shell*]], [*程序启动*], [解析命令，通过 execve / execveat 启动外部程序。],
      [*重定向与管道*], [依赖 pipe2、dup / dup3、openat、close 实现 |、>、< 这些语法。],
      [*作业控制*], [依赖 setpgid、setsid、ioctl(TIOCSPGRP / TIOCGPGRP) 支持前台/后台任务、fg、bg、Ctrl-Z。],
      [*脚本解释器*], [依赖内核在 execve 中解析 shebang。],

      table.cell(rowspan: 4, align: horizon + center)[#text(size: 9.2pt)[*gcc*]], [*阶段调度*], [gcc 本身更像 driver，真正工作交给 cc1、as、ld 这些子程序。],
      [*阶段通信*], [依赖 pipe2、dup / dup3 把预处理、编译、汇编、链接阶段连接起来。],
      [*文件读写*], [依赖 openat、read、write、lseek、pread64 / pwrite64 处理源码、中间文件、目标文件。],
      [*临时产物*], [依赖 fstat / fstatat、statx、unlinkat、renameat2 管理临时产物和输出文件。],

      table.cell(rowspan: 4, align: horizon + center)[#text(size: 9.2pt)[*rustc*]], [*内存映射*], [频繁使用 mmap、munmap、mremap 处理 LLVM 后端和大型编译数据结构。],
      [*权限切换*], [依赖 mprotect 切换映射区域权限，例如 PROT_READ / PROT_EXEC。],
      [*并行编译*], [依赖 futex、set_tid_address、set_robust_list 支撑多线程同步。],
      [*产物缓存*], [可能使用 ftruncate、fallocate、sendfile、getrandom 处理缓存、产物和哈希相关逻辑。],

      table.cell(rowspan: 5, align: horizon + center)[#text(size: 9.2pt)[*vim*]], [*raw 模式*], [核心特征是 ioctl(TCGETS / TCSETS / TCSETSW)，进入编辑模式前保存终端设置，退出时恢复。],
      [*窗口尺寸*], [依赖 ioctl(TIOCGWINSZ) 获取行列数，支撑全屏编辑界面。],
      [*控制终端*], [依赖 ioctl(TIOCGPGRP / TIOCSPGRP / TIOCSCTTY / TIOCGSID) 正确接管终端。],
      [*事件监听*], [依赖 ppoll、pselect6 阻塞接收键盘输入、信号和终端事件。],
      [*文件状态*], [依赖 getdents64、fstat 支持文件浏览、打开、保存这些编辑器功能。],

      table.cell(rowspan: 5, align: horizon + center)[#text(size: 9.2pt)[*git*]], [*工作区操作*], [大量使用 getdents64、mkdirat、unlinkat、linkat、symlinkat、renameat2 管理工作区和 .git 目录。],
      [*pack 读取*], [依赖 mmap、madvise 快速读取和解析大型 pack/index 文件。],
      [*索引锁*], [依赖 flock、fcntl 管理 .git/index.lock 这类锁文件，避免并发破坏仓库状态。],
      [*网络传输*], [clone / pull / push 依赖 socket、connect、sendmsg / recvmsg、setsockopt / getsockopt。],
      [*路径权限*], [依赖 getcwd、chdir、umask、fchmodat 处理仓库根目录、配置和文件权限。],
    )
  ],
  caption: [代表性用户程序及其覆盖的关键内核能力],
) <tab:summary-user-programs>

这 5 类程序共同构成了比单个 benchmark 更严格的验证面。shell 证明内核可以作为用户程序启动中枢；gcc 和 rustc 证明动态链接、文件系统、管道和地址空间管理能够承载大型工具链；vim 证明终端、信号和交互式事件监听具备可用基础；git 则把 VFS、mmap、锁、路径权限和网络连接组合在一起，检验内核对真实开发工作流的支持程度。

== AI 工具使用说明

本项目在 2026 年开发过程中使用了 AI Agent 辅助工程开发。主要工具为 VSCode / Codex 交互环境中的 Codex，相关大模型为 GPT-5。AI 工具的定位是工程协作者：用于阅读代码、定位模块、生成候选补丁、分析日志、整理连续测例结果、维护项目文档草稿。系统设计、需求取舍、最终代码合入、测例是否计入通过、提交记录与答辩材料由参赛队员负责。

为避免 AI 工具零散介入导致工程过程不可追踪，项目建立了一套固定协作流程：

#figure(
  text(size: 8.5pt)[
    #set par(justify: false)
    #table(
      columns: (1.2cm, 2.4cm, 1fr),
      align: (center, left, left),
      inset: (x: 3pt, y: 2.5pt),
      table.header([*阶段*], [*机制*], [*说明*]),
      [1], [*规则入口*], [所有 AI Agent 在行动前先读取 `AGENTS.md`，遵循沟通语言、Git 操作边界、Python 环境、日志保存、scoreboard 更新、危险操作禁令和人工验收规则。],
      [2], [*任务路由*], [根据任务类型读取 `agent_docs/` 下的配套文档：架构定位读取 `project_architecture.md`，构建调试读取 `development_debugging.md`，连续测例状态维护读取 `scoreboard.md` 与 `scoreboard/README.md`。],
      [3], [*计划记录*], [功能开发、性能优化、平台适配和测例修复先在 `plan_docs/` 中建立或更新任务计划，写清目标、阶段、验证点和风险；完成后只添加“已完成，待验收”标记。],
      [4], [*实现与验证*], [AI Agent 可以生成候选代码补丁、Typst 文档草稿和调试命令，但每次修改后必须执行语法检查、构建或窄验证；QEMU 长输出写入 `logs/` 或 `logs/run/`。],
      [5], [*人工验收*], [参赛队员检查 diff、验证日志和实际运行效果后决定是否接受修改；未经人工确认，AI Agent 不提交 commit、不推送远端、不改写历史。],
      [6], [*成果入账*], [若某个连续测例被验证通过，先在 scoreboard 对应架构、libc 和小分文件中写入 `PASS` 及简短依据，再由生成器刷新汇总。],
    )
  ],
  caption: [AI Agent 协作流程],
) <tab:ai-agent-workflow>

这套流程把 AI 工具的贡献分成三类可追踪成果：第一类是任务计划与交互摘要，保存在 `plan_docs/`；第二类是候选实现和文档修改，体现在 git diff 和后续 commit 记录中；第三类是测例验证结果，沉淀在 `logs/`、`logs/run/` 和 `scoreboard/`。因此，AI 工具参与的工作不会绕过人工 review，也不会直接替代参赛队员对正确性、可维护性和比赛诚信披露的责任。

== 总结与后续工作

整体来看，F7LY-OS 2026 年的核心成果是把若干原本分散的内核能力收束成一套面向 Linux 用户态程序的运行环境。进程、线程、地址空间、文件系统、网络和系统调用不再是彼此孤立的模块，而是围绕真实程序的启动、运行、同步、I/O 和退出路径协同工作。当前已经把动态 ELF 装载、musl/glibc loader 路径重写、shebang、VMA / VmObject、futex、epoll、socket_file、SMP 调度、块设备、ext4、pipe 和 VFS `file` 多态体系串成一条完整链路，使内核可以用更接近 Linux 的方式承接复杂应用。

后续工作仍应围绕“减少特殊分支、提高长连续测例稳定性、扩大真实应用覆盖面”展开。首先，LTP 中尚未完全覆盖的项目需要继续按 syscall 族和子系统聚类推进，避免只修单点而破坏其他组合。其次，glibc 侧 libctest 和大型工具链应用对 ABI 细节更敏感，仍需要继续校准 `statx`、termios、socket option、资源限制、命名空间视图和 `/proc` / `/sys` 虚拟文件内容。最后，git 网络、vim 全屏交互、gcc/rustc 完整编译链这些真实工作流应继续从“能启动、能覆盖关键路径”推进到“能稳定完成端到端任务”，同时结合块设备、页缓存、pipe、VMA 和网络调度的进一步优化，把当前原型推向更稳定的日常可用状态。

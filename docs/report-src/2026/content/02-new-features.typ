= 2026 年新增功能

== 双架构统一 VirtIO Block 框架

=== 功能与使用方式

=== 核心对象与执行流程

=== 跨架构 Transport 适配

=== 示例代码与验证结果

// 正文待补：VirtioBlkDevice、VirtioBlkQueue、IoRequest 与双架构截图。

== 预算公平调度与空闲带宽借用

=== 需求与目标

=== Service Class 与 Per-Process Flow

=== 权重预算、同级轮转与带宽借用

=== 研究入口、代码示例与实验结果

// 正文待补：budget-fair 队列图、A/B 吞吐曲线和统计输出。

== 真实 Loopback TCP/UDP

=== TCP 状态与连接建立

=== UDP Datagram Queue

=== 阻塞、非阻塞、背压与信号中断

=== 批量消息、Socket Option 与就绪通知

=== 使用方法、代码示例与网络验证

// 正文待补：TCP/UDP smoke、iperf、netperf 和代表性 socket LTP。

== 双架构交互式 BusyBox Ash

=== Evaluation 与 Shell 双入口

=== UART/SBI、Console 与标准输入

=== Rootfs、环境初始化与正常退出

=== 使用方法、代码示例与终端截图

// 正文待补：make shell r/l、文件访问、脚本执行和 exit。

== 动态 ELF 与 Shebang 执行链

=== PT_INTERP 与动态链接器路径

=== ELF 段对齐与 Auxv

=== Shebang 识别与参数重写

=== 示例代码与执行截图

== Epoll 与事件驱动文件对象

=== Epoll File 与关注集

=== 文件对象就绪接口

=== Timeout、Signal Mask 与等待流程

=== 示例代码与 LTP 结果

== Capability、Timex 与设备兼容层

=== Capability Manager

=== Timex Controller

=== Console Termios

=== File Descriptor 与 Ioctl 兼容对象

== 四组合评测与可复现实验基础设施

=== 四组合测试模型

=== Scoreboard

=== Runner、Parser 与 Ranker

=== 性能研究入口与结果归档

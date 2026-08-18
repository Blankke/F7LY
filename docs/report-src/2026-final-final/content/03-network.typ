= 03 工作二：网络链路

== 网络模块分层

网络模块的目标，是为用户态程序提供 Linux 兼容的 socket 语义，同时保留真实网卡收发能力。我们将网络系统拆成从用户接口到硬件后端的分层结构：上层负责 Linux 用户可见行为，下层负责协议处理、帧收发和设备队列。

整个网络系统主要包含以下几个核心组件：

1. VFS 集成层。socket 被纳入 file 对象和 fd 表，能够参与 `close`、`poll`、`epoll`、`O_NONBLOCK`、`CLOEXEC` 和引用计数管理，并通过 `/proc/net/tcp{,6}` 暴露用户可观察状态。

2. BSD Socket 接口层。该层面向用户程序提供 AF_INET、AF_UNIX 等地址族的 socket、bind、listen、accept、connect、send、recv 和 socket option 等标准入口，负责参数、地址、flags、阻塞语义和 Linux errno。

3. 网络协议栈层。该层基于 Open-NPStack，负责 TCP、UDP、ICMP、IPv4 以及相关以太网协议状态机，处理协议封装、解析和连接状态，不直接依赖具体网卡。

4. 网络适配层。该层在协议栈和平台后端之间提供统一帧收发接口，把协议栈产生的以太网帧交给当前 net backend，也把后端收到的帧交回协议栈处理。

5. 平台驱动层。该层由当前平台画像选择具体实现，RISC-V QEMU 使用 VirtIO MMIO，LoongArch QEMU 使用 VirtIO PCI，实板路径接入 GMAC 等设备。驱动只处理寄存器、DMA / virtqueue、中断和收发队列，不实现 socket 语义。

这套分层的意义在于保持边界清晰：用户程序只看到标准 BSD Socket 和 fd 行为；协议栈只处理网络协议；平台 backend 只处理帧和设备；具体驱动只处理硬件细节。

#figure(
  image("fig/网络分层架构.png", width: 100%),
  caption: [网络分层架构],
) <fig:network-layered-architecture>

== 标准 BSD Socket 接口

F7LY-OS实现了完整的POSIX标准BSD Socket接口，为用户程序提供标准化的网络编程API。通过VFS文件系统抽象，Socket被视为特殊文件，支持统一的文件操作接口，实现了"一切皆文件"的设计理念。

=== 地址族与 socket 类型

当前 socket 层主要面向 AF_INET 与 AF_UNIX，同时保留 IPv6 loopback 和 IPv4-mapped 地址的兼容处理。AF_INET 提供 TCP / UDP 风格的网络通信，AF_UNIX 与 socketpair 服务本机进程间通信。socket 类型决定数据语义：stream socket 表示有连接的字节流，datagram socket 保留报文边界，socketpair 则提供两个已连接端点。

这一层负责检查 domain、type、protocol 和 flags 组合。非法地址族、协议不匹配、用户地址长度错误或不支持的 flags 都应返回 Linux errno；成功创建后，socket 对象进入 fd 表，并继承 `SOCK_NONBLOCK`、`SOCK_CLOEXEC` 等创建时标志。用户程序因此可以用标准 BSD Socket API 建立网络对象。

=== bind / listen / accept4

服务端路径由 `bind -> listen -> accept4` 组成。`bind` 将 socket 绑定到本地地址和端口，并检查地址格式、端口占用和权限边界；`listen` 将 stream socket 转换为监听状态，建立等待连接队列；`accept4` 从队列中取出已建立连接，创建新的 socket file，并根据 flags 设置非阻塞和 close-on-exec 状态。

这组接口的关键是监听 socket 与已连接 socket 的状态分离。监听 fd 继续负责接收新连接，accept 返回的 fd 负责后续数据收发；关闭其中一个对象不能破坏另一个对象的生命周期。

=== connect 与连接状态

客户端通过 `connect` 建立连接。阻塞 socket 在连接完成或失败后返回，非阻塞 socket 可以返回进行中状态，并通过 poll / epoll 观察后续可写或错误事件。内核需要维护 CONNECTING、ESTABLISHED、关闭和错误状态，使后续 `send`、`recv`、`getsockopt` 和 `/proc/net/tcp{,6}` 能看到一致结果。

本机 loopback 连接可以直接在内核中建立 peer 关系；外部 IPv4 连接则交由协议栈处理。无论哪条路径，BSD Socket 接口层向用户暴露的行为都应相同：地址非法返回参数错误，目标不可达返回连接错误，连接完成后 fd 可用于普通读写和 epoll 等待。

=== 数据收发接口

数据收发覆盖 `send` / `recv`、`sendmsg` / `recvmsg`、`sendmmsg` / `recvmmsg` 以及 flags 控制。stream socket 按字节流读写，允许一次读取少于发送长度；datagram socket 保留报文边界，一次 recv 取出一个报文。`MSG_DONTWAIT`、`MSG_MORE`、`MSG_NOSIGNAL` 等 flags 会影响阻塞、发送聚合或错误通知，但不能绕过 socket 状态机。


=== socket option 与 fd 行为

`setsockopt` / `getsockopt` 维护 socket 的协议和通用选项，例如地址复用、接收缓冲、发送缓冲、错误状态和 TCP / UDP 相关配置。即使某些 option 在当前阶段只是受限实现，也必须明确返回成功、当前值或对应错误，避免 glibc 程序因不可解释的结果走入异常分支。

socket 作为 fd 还要遵守普通文件描述符行为。`O_NONBLOCK` 可以通过创建 flags 或 fcntl 改变；`CLOEXEC` 决定 exec 成功后是否关闭；fork 后 fd 表引用要保持 socket 对象存活；最后一个引用 close 时，才释放端口登记、peer 引用、等待队列和 `/proc/net` 状态。poll / epoll 只通过 ready 接口观察可读、可写、错误和关闭状态。

=== 与 Linux 可观察状态闭合

BSD Socket 接口的结果不仅体现在 syscall 返回值，也体现在后续可观察状态。监听 socket 应能在 `/proc/net/tcp{,6}` 中表现为 LISTEN，连接建立后表现为 ESTABLISHED；关闭、半关闭、错误和非阻塞状态要影响 poll / epoll ready 与后续 `recv` / `send` 返回值。

#figure(
  image("fig/网络模块——标准BSD Socket接口.png", width: 100%),
  caption: [标准BSD Socket接口],
) <fig:network-bsd-socket-interface>

== 网络协议栈与VirtIO-net

网络模块以 Linux 兼容 Socket 为统一入口，向下分为两条数据路径。

1. 本机通信走 socket_file loopback 快路径，直接通过端口表、peer 连接和缓冲队列传递 TCP/UDP payload；

2. 外部 IPv4 流量交由 ONPS 协议栈处理TCP/UDP/ICMP/IP，再通过 VirtIO-Net 收发完整以太网帧。

这种设计兼顾可验证的本机数据面与后续外网扩展能力：CAgent 和 LTP 可以稳定验证 loopback、socket 状态和 `/proc/net` 语义；外部网络能力则沿 ONPS -> net backend -> VirtIO-net / GMAC 的链路继续扩展。

#figure(
  image("fig/网络协议栈与VirtIO-net.png", width: 100%),
  caption: [网络协议栈与VirtIO-net],
) <fig:overview-layered-architecture>

=== 协议栈职责

Open-NPStack 在本阶段承担外部 IPv4 路径中的 TCP、UDP、ICMP、IP 以及以太网协议状态机。它负责报文封装与解析、端口和连接状态维护、重传和校验和等协议行为，但不直接接触用户态 fd，也不直接操作 VirtIO 或 GMAC 寄存器。

来自 socket 层的外部 IPv4 请求会进入协议栈，由协议栈决定是建立连接、发送数据报还是更新状态；来自网卡的 RX 帧则先进入协议栈解析，再转化为 socket 能理解的接收数据或连接变化。这样，协议处理和用户接口之间形成明确分层。

=== VirtIO-net 数据路径

VirtIO-net 在 F7LY-OS 中作为 QEMU 网络的主要数据面。RISC-V QEMU 使用 VirtIO MMIO，LoongArch QEMU 使用 VirtIO PCI；它们都通过统一的 net backend 接入协议栈。驱动负责初始化队列、收发 descriptor、TX 提交、RX 回收和中断通知，上层协议栈只看到“可发帧 / 可收帧”的接口。

VirtIO-net 的作用是把协议栈生成的以太网帧送到虚拟网卡，再把虚拟网卡收到的帧交回协议栈。它不关心 socket 是由哪个用户程序创建的，也不关心当前数据属于 TCP 还是 UDP，只负责把帧在协议栈和设备之间可靠传递。

=== 平台网卡与外部网络

在实板上，网络链路接入 GMAC 等真实网卡。平台画像决定使用哪种硬件后端，网络适配层把协议栈和网卡驱动隔离开，驱动只处理 DMA ring、寄存器、中断和收发队列。这样，QEMU 路径与实板路径可以共享相同的协议栈和 socket 语义，只在最后一层设备后端不同。

外部网络能力的验证重点不是某个驱动内部细节，而是“从 socket 到帧、从帧到 socket”的闭环是否成立。CAgent、LTP 以及后续真实网络测试最终看到的，是协议栈能否正确维护连接状态、是否能通过 VirtIO-net 或 GMAC 完成收发，以及 `/proc/net/tcp{,6}` 是否与用户态行为一致。

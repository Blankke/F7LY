= 网络系统模块

== 网络分层与帧收发边界

网络模块采用“ONPS 协议栈 -> 公共帧收发接口 -> 当前平台 net backend -> 具体网卡”的方向。ONPS 负责 Ethernet、ARP、IPv4、ICMP、TCP 和 UDP 的协议状态机；公共接口只传递帧缓冲、长度和收发结果；平台后端负责把这些帧交给 VirtIO 或 GMAC/JH7110 的 DMA 队列。

```cpp
struct NetBackend {
  bool init(const NetworkConfig &config);
  int transmit(const uint8 *frame, size_t length);
  int poll_receive(uint8 *frame, size_t capacity);
};

NetBackend *backend = platform_network_backend();
onps_register_frame_io(backend);
```

RISC-V QEMU 使用 VirtIO MMIO，LoongArch QEMU 使用 VirtIO PCI，VisionFive2 和 2K1000 画像接入各自的板级网卡。`AF_UNIX` 和 loopback 通信在内核内部完成，不依赖这些网卡；访问外部 IPv4 地址时，数据才会进入对应的网卡后端。这样，QEMU 可以单独验证 socket、TCP 和 UDP 的基础行为，网卡驱动只负责它实际承担的外部收发路径。

== loopback TCP/UDP 数据面

决赛工作负载大量使用本机服务、测试工具和 socketpair。为此，`socket_file` 提供了真实的内核内 loopback 数据面：监听端口、连接建立、收发队列和对端引用都在内核中完成。loopback 实现了用户可见的 TCP/UDP 语义，并且是 CAgent 与 iperf/netperf 验证网络能力的实际路径。

TCP 的状态转换由 socket 文件统一维护。典型路径是 `socket -> bind -> listen -> accept -> connect`；`accept` 创建新的已连接 socket，连接两端互相保存 peer 引用，发送数据进入对端接收队列，关闭时再按半关闭和最终释放规则拆除引用。UDP 不建立连接，`bind` 登记本地端口，`sendto` 生成独立数据报并把来源地址一并放入接收队列；`recvfrom` 取出一个完整数据报，避免把多次发送错误拼成一个字节流。

```cpp
int socket_file::loopback_send(const void *buf, size_t len) {
  auto *peer = _peer.load();
  if (peer == nullptr || peer->is_closing()) return -EPIPE;
  return peer->enqueue_recv(buf, len);
}
```

收发队列的锁和等待队列遵循统一的阻塞规则：队列为空时，阻塞接收进入等待；有数据或对端关闭时唤醒；`O_NONBLOCK` 直接返回 `-EAGAIN`。小块 stream I/O 在成功传输后主动让出一次 CPU，避免单个线程连续执行大量 `send/recv` 把其他 runnable 任务饿死。`MSG_MORE`、`sendmmsg` 和 `recvmmsg` 则在这个数据面上实现批量提交或延迟发送，不另造一套 socket 状态。

IPv6 当前主要承担 loopback 和 IPv4-mapped 兼容：`::1`、未指定地址以及映射地址在 socket 层转换到相同的本地端口表。这样 glibc 程序使用 IPv6 socket 访问本机服务时仍能得到一致的连接和错误结果；完整的外部 IPv6 路由不作为本阶段 loopback 证据。

== ONPS 与平台网卡后端

非 loopback IPv4 流量进入 ONPS。`f7ly_network` 在第一次创建相关 socket 时初始化协议栈和平台后端，socket 层只根据地址类型选择 loopback 或 ONPS 路径。ONPS 的线程、定时器和缓冲区通过 `kernel/net/onpstack/port` 适配到 F7LY 的线程、信号量和定时器接口；适配层负责生命周期转换，不把 ONPS 的内部句柄暴露给用户态。

VirtIO 后端完成 MAC 读取、virtqueue 建立、TX 提交和 RX 回收；GMAC 后端完成板级 DMA 描述符和中断接入。两者最终都调用公共帧接口，因此协议栈和 socket 系统调用无需按架构分支。RX 路径回收缓冲区后再交给 ONPS，TX 路径在设备确认后释放发送缓冲区，避免高并发发送时出现缓冲快速耗尽或重复释放。

== socket 生命周期与 Linux 状态接口

网络状态不仅体现在 `connect` 是否返回成功，也要能被用户通过 `/proc/net/tcp` 和 `/proc/net/tcp6` 读取。内核为每个 TCP socket 登记本地地址、远端地址、端口和状态；登记节点嵌入 socket 对象并由受保护的链表连接，因而不再依赖固定容量的全局数组。创建、监听、连接、关闭和最终文件引用释放都在同一把登记锁下更新，快照生成时只复制必要字段，随后在不持锁的情况下格式化文本。

```text
sl  local_address rem_address   st
01:0100007F:1F90 00000000:0000 0A   # LISTEN
02:0100007F:1F90 0100007F:C001 01   # ESTABLISHED
```

其中 `0A`、`01` 等状态值遵循 Linux `/proc/net/tcp` 约定，地址和端口按 ABI 要求编码。监听 socket 在 `listen` 完成后可观察为 LISTEN，连接建立后更新为 ESTABLISHED，连接尚在建立或关闭过程中则分别反映 CONNECTING、FIN_WAIT 等状态。TCP6 使用同一生命周期登记，只改变地址格式和表头，不复制一套并发管理逻辑。

它把内部 socket 状态机与 Linux 用户态直接连接起来：CAgent 可以通过读取文件确认监听和连接是否真的发生，诊断工具也能区分“系统调用成功但状态未更新”和“协议连接确实建立”。本阶段的定向回归覆盖 TCP/UDP loopback、socket 状态查询以及双架构 CAgent network 路径；结果显示 RV 和 LoongArch 的网络用例连续通过。

== 小结

本阶段网络优化形成了清晰的责任链：协议栈处理协议，公共接口处理帧，平台 backend 处理设备，`socket_file` 处理 Linux socket 语义。实际验证集中在真实 loopback TCP/UDP、批量消息、阻塞与关闭行为以及 `/proc/net/tcp{,6}` 状态快照；ONPS 到 VirtIO/GMAC 的外网通路则以统一适配边界接入。

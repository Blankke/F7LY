= 第九章　网络系统模块

== 9.1　网络系统架构概述

网络系统是用户程序与外部主机进行通信的重要基础设施，允许用户程序通过 Socket 接口完成数据发送、接收、连接建立、端口监听、事件等待等操作。F7LY 实现了与 Linux ABI 兼容的网络系统调用接口，并在 2026 年进一步完善本机 TCP/UDP 数据面能力，使网络子系统从早期的协议栈框架与 Socket 接口占位，推进到能够真实传递 payload 的可用阶段。目前，F7LY 已支持 `send`/`recv`、`sendto`/`recvfrom`、`poll`/`epoll` 等关键接口，能够支撑 `iperf`、`netperf`、BusyBox 网络工具以及部分 LTP 网络用例完成真实的数据传输与就绪通知验证。

当前网络模块分为三层：

- 第一层是 Linux 兼容的 BSD Socket 接口。系统调用层负责参数获取、用户态地址拷贝、fd 分配和错误码转换，真正的 socket 状态机集中在 `socket_file` 中。
- 第二层是内核内 loopback 数据面。`127.0.0.1`、`0.0.0.0` 以及可映射的 IPv6 loopback 地址不经过网卡，也不经过完整 Ethernet/IP 层，而是在内核内通过端口表、peer 指针和接收队列传递数据。
- 第三层是 ONPS 与 VirtIO-Net 适配后端。非 loopback 的 IPv4 TCP/UDP/ICMP 流量在网络栈初始化成功后可以交给 ONPS，ONPS 再通过 `virtio0` 适配层收发完整以太网帧。

#figure(
  image("fig/网络模块.png", width: 100%),
  caption: [网络模块架构示意图],
) <fig:net-architecture>

图 @fig:net-architecture 展示了这一分层关系。需要特别说明的是，F7LY 采用本机 loopback 与 ONPS/VirtIO-Net 双路径框架，是基于实现边界清晰和功能验证稳定性的考虑。这样的划分避免了两个问题：一方面，localhost 通信不必受真实网卡、QEMU user 网络、ARP 或外部路由状态影响；另一方面，外部网络仍然保留了从协议栈到 VirtIO 设备的完整接入点，后续可以继续扩展。

网络栈初始化采用懒加载方式。第一次创建 IPv4 TCP/UDP/ICMP socket 时，`sys_socket()` 会尝试调用 `net::init_network_stack()`。

```cpp
bool init_network_stack()
{
    if (network_initialized) {
        return true;
    }

    EN_ONPSERR onps_error;
    if (!open_npstack_load(&onps_error)) {
        return false;
    }

    if (!net::adapter_init()) {
        open_npstack_unload();
        return false;
    }

    network_initialized = true;
    return true;
}
```

这段初始化顺序体现了网络模块的两个边界。`open_npstack_load()` 只负责启动 ONPS 协议栈核心，包括 buddy、buf_list、定时器、输入线程、网络接口表和路由表等基础结构；`adapter_init()` 才负责把底层 VirtIO-Net 设备注册成 ONPS 可见的以太网接口。Socket 层在转发前通过 `should_route_via_onps()` 判断目标地址是否为 loopback，从而决定走内核内路径还是 ONPS 后端路径。

== 9.2　核心网络协议栈与 loopback 数据面

本节重点分析F7LY的 loopback实现。当前loopback实现方式为内核内部的 socket_file 层直接处理本机通信，绕过 VirtIO 网卡和完整的 Ethernet/IP 层。这种实现是基于性能和实现复杂度的考虑。实现时保留了 ONPS 的loopback模块, 以确保后续能够无缝切换到 ONPS 后端。

实现思路为：本机通信不经过 VirtIO 网卡，也不经过 Ethernet/IP 层。用户态看到的仍然是标准 BSD Socket 接口，但内核内部会把 loopback 流量留在 `socket_file` 层，通过端口表、peer 指针、接收缓冲区和等待队列完成传输。

整体路径可以概括为：

```text
TCP loopback:
socket/connect -> 查找 listener -> 创建 server-side socket
               -> client/server-side 互设 peer
               -> send 写入 peer->_recv_buffer
               -> recv 从本端 _recv_buffer 读取

UDP loopback:
socket/bind -> 注册 UDP 端口
sendto      -> 按目标端口查找 receiver
            -> 入队一枚 loopback_datagram
recvfrom    -> 出队 datagram 并回填源地址
```

这一实现和真实网卡路径是分开的。`socket_file` 先判断目标地址是否属于本机通信；只有目标不是 loopback/any，且网络栈已经初始化成功时，才把流量转交给 ONPS。

```cpp
bool is_loopback_or_any(uint32 addr)
{
    return addr == 0 || addr == k_loopback_addr || addr == 0x7f000001;
}

bool should_route_via_onps(SocketFamily family, SocketType type, uint32 addr)
{
    return can_use_onps_socket(family, type) && !is_loopback_or_any(addr);
}
```

=== loopback 端口表

loopback 数据面的入口是本机端口表。F7LY 用 `g_loopback_bindings` 记录已经绑定的 TCP/UDP 端口，表项由 socket 类型和端口号共同确定。TCP listener 和 UDP receiver 都通过这张表被找到。没有显式 `bind()` 的 socket 会在 `connect()`、`listen()` 或首次 `sendto()` 前自动分配临时端口，范围是 `20000..60999`。

```cpp
constexpr int k_loopback_binding_max = 256;
constexpr uint16 k_ephemeral_port_start = 20000;
constexpr uint16 k_ephemeral_port_end = 60999;

struct loopback_binding
{
    bool used = false;
    uint16 port = 0;
    SocketType type = SocketType::TCP;
    socket_file *socket = nullptr;
};

SpinLock g_loopback_lock;
loopback_binding g_loopback_bindings[k_loopback_binding_max];
```

端口表解决的是“本机路由”问题：TCP `connect()` 用目标端口找到正在 `listen()` 的 socket；UDP `sendto()` 用目标端口找到接收端 socket；`accept()` 则从 listener 自己的 pending 队列中取出已经建立好的 server-side socket。这样，loopback 后端不需要构造 IP 包，也不需要经过 ARP、路由表和网卡队列。

=== TCP loopback 方法

TCP loopback 的核心是成对 `socket_file`。客户端调用 `connect()` 时，内核按目标端口查找 listener。如果服务端线程还在启动过程中，`connect()` 会短暂等待 listener 完成 `bind/listen`，以降低 netperf 这类程序中 server/client 启动竞态的影响。

找到 listener 后，内核创建一个新的 server-side `socket_file`，将它与 client socket 互相挂为 `_peer`，再把 server-side 放入 listener 的 `_pending_connections` 队列。服务端 `accept()` 取出的不是原 listener，而是这个已经与客户端互连的新 socket。

```cpp
class socket_file : public file
{
private:
    eastl::vector<socket_file*> _pending_connections;
    int _backlog;
    socket_file *_peer;

    eastl::vector<uint8_t> _recv_buffer;
    eastl::vector<uint8_t> _send_buffer;
    bool _read_shutdown;
    bool _write_shutdown;
    bool _peer_closed;
    bool _peer_write_shutdown;
};
```

连接建立后，TCP 数据发送不再创建网络包，而是直接写入对端 socket 的接收缓冲区。TCP 是字节流协议，所以 `_recv_buffer` 不保存单次 `send()` 的边界；接收方每次 `recv()` 只是从字节流头部取走指定长度的数据。

为了避免单个连接无限占用内核内存，TCP loopback 给每个接收缓冲区设置了 512KB 上限。发送方发现对端缓冲区满时，阻塞 socket 会睡眠等待；非阻塞 socket 或带 `MSG_DONTWAIT` 的发送会返回 `-EAGAIN`。接收方读出数据后唤醒写端，形成一个明确的背压闭环。

```cpp
constexpr size_t k_tcp_recv_buffer_max_bytes = 512 * 1024;

int socket_file::enqueue_stream_data_to_peer(socket_file *peer,
                                             const uint8_t *data,
                                             size_t len,
                                             bool nonblocking)
{
    size_t queued = 0;
    while (queued < len) {
        peer->_lock.acquire();
        while (peer->_recv_buffer.size() >= k_tcp_recv_buffer_max_bytes &&
               peer->stream_receive_open_locked()) {
            if (nonblocking) {
                peer->_lock.release();
                return queued > 0 ? static_cast<int>(queued) : -EAGAIN;
            }
            proc::k_pm.sleep(&peer->_recv_buffer, &peer->_lock);
        }

        size_t used = peer->_recv_buffer.size();
        size_t space = used < k_tcp_recv_buffer_max_bytes
                           ? k_tcp_recv_buffer_max_bytes - used
                           : 0;
        size_t chunk = eastl::min(len - queued, space);
        size_t old_size = peer->_recv_buffer.size();
        peer->_recv_buffer.resize(old_size + chunk);
        memcpy(peer->_recv_buffer.data() + old_size, data + queued, chunk);
        queued += chunk;

        proc::k_pm.wakeup(&peer->_recv_buffer);
        peer->_lock.release();
    }
    return static_cast<int>(queued);
}
```

在基本字节流之外，TCP loopback 还实现了用户态程序常见的边界语义：

- `MSG_MORE` 会先把数据暂存在 `_send_buffer`，等下一次非 `MSG_MORE` 发送时再合并写入对端；
- `shutdown(SHUT_RD/SHUT_WR)` 会分别关闭读半边或写半边，并唤醒对端，使 `recv()` 能正确看到 EOF 或错误；
- `SO_RCVTIMEO` 参与阻塞接收的 deadline 计算，等待超时后返回对应错误；
- 等待期间收到未屏蔽信号时，阻塞系统调用返回 `-EINTR`；
- `read_ready()`、`write_ready()` 和 `epoll_rdhup_ready()` 对接 `poll/select/epoll`。

因此，F7LY 的 TCP loopback 不只是把两个 socket 粘在一起，而是实现了连接队列、流式缓冲、背压、半关闭、超时、信号中断和就绪通知这些真实程序会依赖的行为。

=== UDP loopback 方法

UDP loopback 与 TCP 的最大区别是必须保留报文边界。一次 `sendto()` 对应接收端队列中的一枚 datagram，`recvfrom()` 一次只取一枚，并把源地址回填给用户态。

```cpp
struct loopback_datagram
{
    struct sockaddr_in src_addr;
    eastl::vector<uint8_t> data;
};

eastl::vector<loopback_datagram> _datagram_queue;
size_t _datagram_queue_bytes;
```

发送时，`socket_file` 先确定目标地址。如果目标是非 loopback IPv4，并且 ONPS 网络栈可用，则走出网路径；否则按目标端口在 `g_loopback_bindings` 中查找 UDP receiver。找到接收者后，内核把源地址和 payload 打包成一枚 `loopback_datagram`，压入接收端 `_datagram_queue` 并唤醒阻塞的 `recvfrom()`。

UDP 队列同样有上限：最多 256 个报文，或最多 256KB payload。队列满时当前实现会丢弃新到达的 datagram，但 `sendto()` 仍按 UDP 语义返回成功。这一点很重要：UDP 本身不是可靠协议，loopback 后端不能因为在内核内实现就把它改造成可靠队列。

```cpp
constexpr size_t k_udp_queue_max_bytes = 256 * 1024;
constexpr size_t k_udp_queue_max_packets = 256;

int socket_file::enqueue_datagram(const struct sockaddr_in *src_addr,
                                  const uint8_t *data,
                                  size_t len)
{
    if (_datagram_queue.size() >= k_udp_queue_max_packets ||
        _datagram_queue_bytes + len > k_udp_queue_max_bytes) {
        return static_cast<int>(len);
    }

    loopback_datagram packet;
    if (src_addr != nullptr) {
        packet.src_addr = *src_addr;
    }
    packet.data.resize(len);
    if (len > 0) {
        memcpy(packet.data.data(), data, len);
    }
    _datagram_queue_bytes += len;
    _datagram_queue.push_back(packet);
    proc::k_pm.wakeup(&_datagram_queue);
    return static_cast<int>(len);
}
```

UDP 还支持 connected UDP 语义。调用 `connect()` 后，socket 会记录 remote 地址，后续 `send()` 可以复用这个目标；接收侧仍然保持 datagram 队列和源地址回填。若目标端口没有接收者，`sendto()` 按 UDP 语义允许成功返回，而不是强制要求连接存在。

=== IPv6 loopback 兼容

F7LY 目前没有完整 IPv6 协议栈，但许多用户态程序会优先创建 AF_INET6 socket 做双栈监听。为兼容这类程序，`socket_file` 只支持把本机相关 IPv6 地址映射到 IPv4 loopback：

- `::` 映射为 `0.0.0.0`；
- `::1` 映射为 `127.0.0.1`；
- IPv4-mapped IPv6 地址映射为其尾部 IPv4 地址。

不可映射的 IPv6 地址直接返回不支持。这个边界比较清晰：当前实现不是完整 IPv6 网络，而是让双栈 localhost 程序复用已经实现的 IPv4 loopback 数据面。

== ONPS 与 VirtIO 出网路径

本节说明非 loopback IPv4 流量如何走出内核内数据面。F7LY 在出网路径上复用 ONPS 作为 TCP/IP 协议栈后端，再通过 VirtIO-Net 适配层把 ONPS 的以太网接口接到 QEMU 暴露的虚拟网卡上。换言之，loopback 是 F7LY 自己实现的本机数据面；出网路径则是 `socket_file -> ONPS -> virtio0 -> VirtIO-Net`。

VirtIO-Net 驱动承担的是“收发完整以太网帧”的职责。它不理解 TCP 连接、UDP 报文或 socket 状态，也不直接接触用户态 fd；上层 ONPS 给它一个已经组装好的 Ethernet frame，它把帧放入 TX virtqueue 交给设备；设备收到包后写入 RX virtqueue，它取出帧并交回 ONPS。

F7LY 在双架构下使用不同的 VirtIO 传输方式：

- RISC-V 平台扫描 `VIRTIO_MMIO_FIRST` 到 `VIRTIO_MMIO_LAST` 之间的 MMIO 槽位，查找 `device_id = 1` 的网络设备；
- LoongArch 平台通过 PCI 探测 `VIRTIO_NET_VENDOR_ID` 和 `VIRTIO_NET_DEVICE_ID`，再配置 modern PCI virtqueue；
- 两条路径最终都初始化同一份 `virtio_net_device` 状态，包括 RX/TX 队列、描述符数组、available ring、used ring、MAC 地址和设备锁。

驱动当前只协商 `VIRTIO_NET_F_MAC`。校验和卸载、TSO、mergeable buffer 等复杂特性没有启用，原因是 F7LY 的网络目标不是在这一层做性能优化，而是先保证协议栈能够稳定收发完整帧，并让上层对数据边界和错误语义有确定行为。

```cpp
uint64 negotiate_features(uint64 device_features)
{
    // 当前驱动只收发完整以太网帧，不启用校验和、TSO、mergeable buffer。
    return device_features & (1ULL << VIRTIO_NET_F_MAC);
}
```

=== RX/TX 队列组织

VirtIO-Net 使用两个 virtqueue：RX 队列由设备写入，TX 队列由驱动写入。每个队列当前使用 `NUM_NET_DESC = 32` 个描述符。初始化时驱动会一次性向 RX 队列投递全部可写 buffer，否则设备即使收到网络包也没有 DMA 目标。

发送路径的关键步骤是：

1. 主动回收已经完成的 TX used ring，释放旧 descriptor；
2. 找到空闲 TX descriptor；
3. 在包前写入清零的 `virtio_net_hdr`；
4. 将完整以太网帧复制到 header 后方；
5. 把 descriptor index 放入 avail ring；
6. notify TX queue。

```cpp
int virtio_net_send(const void *data, uint32 len)
{
    process_tx_used_locked();

    virtio_net_hdr *hdr =
        reinterpret_cast<virtio_net_hdr *>(g_net.tx_buffers[buf_idx].data);
    memset(hdr, 0, sizeof(*hdr));
    memcpy(g_net.tx_buffers[buf_idx].data + k_net_header_len, data, len);

    g_net.tx_desc[desc_idx].addr = dma_addr(g_net.tx_buffers[buf_idx].data);
    g_net.tx_desc[desc_idx].len = len + k_net_header_len;
    g_net.tx_desc[desc_idx].flags = 0;

    g_net.tx_avail[2 + (idx % NUM_NET_DESC)] = desc_idx;
    __sync_synchronize();
    set_avail_idx(g_net.tx_avail, idx + 1);
    notify_queue(VIRTIO_NET_TX_QUEUE_IDX);
    return 0;
}
```

接收路径则从 RX used ring 中取出设备已经写好的 descriptor，去掉前置的 `virtio_net_hdr`，把后面的以太网帧复制给上层，然后立即把同一个 descriptor 重新投递回 RX 队列。这样 RX buffer 始终处于循环复用状态。

```cpp
int virtio_net_recv(void *data, uint32 *len)
{
    if (g_net.rx_used_idx == g_net.rx_used->idx) {
        return -1;
    }

    VRingUsedElem *elem = &g_net.rx_used->ring[g_net.rx_used_idx % NUM_NET_DESC];
    uint32 desc_idx = elem->id;
    uint32 used_len = elem->len;
    ++g_net.rx_used_idx;

    uint32 data_len = used_len - k_net_header_len;
    memcpy(data, g_net.rx_buffers[buf_idx].data + k_net_header_len, copy_len);

    g_net.rx_free[desc_idx] = 1;
    post_rx_desc_locked(desc_idx);
    notify_queue(VIRTIO_NET_RX_QUEUE_IDX);
    return 0;
}
```

这一路径保持了驱动层的职责单一：只处理 DMA、virtqueue、MAC 和帧边界，不把 socket 语义泄漏到底层设备中。

=== ONPS 适配层

ONPS 原本需要一个以太网设备抽象，而 F7LY 的 VirtIO-Net 驱动暴露的是 `virtio_net_send()` 和 `virtio_net_recv()`。两者之间由 `virtio_net_adapter.cc` 衔接。适配层在初始化时读取 VirtIO MAC 地址，构造 QEMU user-mode 网络常用配置，并通过 `ethernet_add()` 注册接口：

```cpp
ST_IPV4 ipv4_config;
ipv4_config.unAddr = inet_addr("10.0.2.15");
ipv4_config.unSubnetMask = inet_addr("255.255.255.0");
ipv4_config.unGateway = inet_addr("10.0.2.2");
ipv4_config.unPrimaryDNS = inet_addr("10.0.2.3");
ipv4_config.unBroadcast = inet_addr("10.0.2.255");

onps_netif = ethernet_add("virtio0",
                          mac_addr,
                          &ipv4_config,
                          virtio_emac_send,
                          start_recv_thread_wrapper,
                          &onps_netif,
                          &error);
```

发送方向上，ONPS 给出的不是连续内存，而是内部 `buf_list` 链表。`virtio_emac_send()` 先计算总长度，再将链表合并到一块连续的 `tx_packet_buffer`，最后调用 `virtio_net_send()`。接收方向上，适配层创建一个名为 `virtio-net-rx` 的内核线程，循环调用 `virtio_net_recv()`。一旦取到完整以太网帧，就通过 `ethernet_ii_recv(netif, rx_packet_buffer, packet_len)` 交还给 ONPS 解析。

```cpp
int virtio_emac_send(short buf_list_head, unsigned char *error)
{
    UINT total_len = buf_list_get_len(buf_list_head);
    if (total_len <= 0 || total_len > ETH_FRAME_LEN) {
        return -1;
    }

    buf_list_merge_packet(buf_list_head, tx_packet_buffer);
    return net::virtio_net_send(tx_packet_buffer, total_len) == 0
               ? static_cast<int>(total_len)
               : -1;
}
```

适配层的价值在于隔离两种模型：ONPS 仍然按照自己的网卡回调、buf_list 和 `ethernet_ii_recv()` 工作；F7LY 驱动层仍然按照 VirtIO queue 和连续 DMA buffer 工作。二者之间没有互相侵入，也没有把 ONPS 的内部结构扩散到设备驱动中。

== 9.4　BSD Socket 接口与系统调用集成

网络模块对用户态暴露的是 Linux 风格 BSD Socket ABI。F7LY 没有为 socket 建立一套独立于文件系统的对象体系，而是让 `socket_file` 继承 VFS 的 `file` 基类。这一点非常关键：socket 一旦成为普通 fd，就可以自然参与 `read`/`write`、`close`、`dup`、`fcntl`、`poll`、`select`、`epoll`、`fork` 后 fd 继承等通用路径。

```cpp
class socket_file : public file
{
public:
    virtual long read(uint64 buf, size_t len, long off, bool upgrade) override;
    virtual long write(uint64 buf, size_t len, long off, bool upgrade) override;
    virtual bool read_ready() override;
    virtual bool write_ready() override;

    int bind(const struct sockaddr *addr, socklen_t addrlen);
    int listen(int backlog);
    int accept(struct sockaddr *addr, socklen_t *addrlen,
               socket_file **accepted_socket);
    int connect(const struct sockaddr *addr, socklen_t addrlen);
    int sendmsg(const struct msghdr *msg, int flags);
    int recvmsg(struct msghdr *msg, int flags);
};
```

在 VFS 视角下，socket 和 pipe、普通文件、设备文件一样都是 `file`。区别只在于各自的后端实现不同：普通文件读写 ext4/FAT32，管道读写环形缓冲区，socket 则根据地址和协议族选择 loopback、AF_UNIX 或 ONPS 后端。

=== 系统调用层职责

系统调用层主要承担 ABI 边界工作，而不直接实现协议状态机。以 `socket()` 为例，`sys_socket()` 会拆出 `SOCK_CLOEXEC` 和 `SOCK_NONBLOCK`，校验协议族、类型和协议号，然后创建 `socket_file` 并放入当前进程 fd 表。支持的协议族包括 `AF_UNIX`、`AF_INET` 和用于 loopback 兼容的 `AF_INET6`；支持的类型包括 `SOCK_STREAM`、`SOCK_DGRAM`、`SOCK_RAW`，其中 AF_UNIX 的 `SOCK_SEQPACKET` 在内部复用可靠本地 stream 语义。

```cpp
uint64 SyscallHandler::sys_socket()
{
    int domain, type, protocol;
    if (_arg_int(0, domain) < 0 ||
        _arg_int(1, type) < 0 ||
        _arg_int(2, protocol) < 0) {
        return SYS_EINVAL;
    }

    int fd_flags = type & (O_CLOEXEC | O_NONBLOCK);
    int base_type = type & ~(O_CLOEXEC | O_NONBLOCK);

    if (domain == AF_INET &&
        (base_type == SOCK_STREAM || base_type == SOCK_DGRAM || base_type == SOCK_RAW) &&
        !net::is_network_stack_ready()) {
        net::init_network_stack();
    }

    fs::socket_file *sock = new fs::socket_file(domain, base_type, protocol);
    sock->lwext4_file_struct.flags = O_RDWR | fd_flags;
    sock->set_nonblock((fd_flags & O_NONBLOCK) != 0);

    proc::Pcb *p = proc::k_pm.get_cur_pcb();
    int fd = proc::k_pm.alloc_fd(p, sock);
    if (fd >= 0 && (fd_flags & O_CLOEXEC) != 0) {
        p->_ofile->_fl_cloexec[fd] = true;
    }
    return fd;
}
```

`bind()`、`listen()`、`connect()`、`accept()`、`sendto()`、`recvfrom()` 等调用同样遵循这个分工。syscall handler 负责 fd 类型检查、`copy_in`/`copy_out`、sockaddr 长度校验和返回值包装；地址归一化、端口登记、listener 查找、队列入队、阻塞等待和错误语义由 `socket_file` 统一处理。这样可以避免 syscall 层和 socket 后端同时修改状态，降低竞态和语义分叉。

当前已绑定的主要网络系统调用包括：

- 连接管理：`socket`、`socketpair`、`bind`、`listen`、`accept`、`accept4`、`connect`、`shutdown_socket`；
- 地址查询：`getsockname`、`getpeername`；
- 数据收发：`sendto`、`recvfrom`、`sendmsg`、`recvmsg`、`sendmmsg`、`recvmmsg`、`recvmmsg_time64`；
- 选项与控制：`setsockopt`、`getsockopt`、socket 相关 `ioctl`。

=== socket_file 的状态机

`socket_file` 用 `SocketState` 描述生命周期：`CREATED`、`BOUND`、`CONNECTING`、`LISTENING`、`CONNECTED`、`CLOSED`。不同协议使用同一套外层状态，但内部队列不同：TCP 使用 `_pending_connections` 和 `_recv_buffer`，UDP 使用 `_datagram_queue`，AF_UNIX 使用路径表或 `socketpair()` 直接建立 peer。

```cpp
enum class SocketState
{
    CREATED,
    BOUND,
    CONNECTING,
    LISTENING,
    CONNECTED,
    CLOSED
};
```

`read()` 和 `write()` 在 socket 上分别映射到 `recv()` 和 `send()`。这保证了大量只使用文件接口的用户态代码也能操作 socket，例如 shell 管道重定向、库函数封装或某些测试程序中的通用 fd 读写逻辑。

就绪通知同样放在 `socket_file` 中实现。TCP 读就绪取决于接收缓冲区是否有数据、对端是否关闭写半边或本端是否已进入 EOF；UDP 读就绪取决于 datagram 队列是否非空；listener 读就绪取决于 pending 连接队列是否有成员。写就绪则要考虑 socket 是否连接、写半边是否关闭、peer 是否存在，以及 TCP loopback 对端接收缓冲是否低于 512KB 上限。

这种设计与第七章的 epoll 实现天然衔接：`epoll_file` 不需要理解网络协议，只要回调被关注 fd 的 `read_ready()` 和 `write_ready()` 即可。

=== 批量消息与选项兼容

为了适配真实用户态网络程序，F7LY 补齐了 `sendmsg`、`recvmsg`、`sendmmsg` 和 `recvmmsg`。`sendmsg`/`recvmsg` 负责处理 iovec 数组和可选地址；批量接口则循环调用单条消息路径。`recvmmsg` 有一个重要细节：第一条消息可以阻塞等待，但读到第一条后会给后续消息自动加上 `MSG_DONTWAIT`，避免用户给出较大的 `vlen` 时在队列被读空后永久卡住。

socket option 方面，loopback 后端对常见选项采取“能产生真实行为的真实实现，调优类选项稳定 no-op”的策略。`SO_RCVTIMEO` 会影响阻塞接收等待，`SO_SNDTIMEO` 会被保存并可由用户态查询，`SO_REUSEADDR` 会影响绑定行为；`TCP_NODELAY`、`TCP_CORK`、部分缓冲区大小和拥塞控制查询在 loopback 中没有真实网卡或拥塞窗口语义，因此主要返回兼容值，保证用户态库不会因为选项缺失而失败。

网络 ioctl 由 `SocketIoctlCompat` 提供兼容视图。当前它只暴露内核确实能模拟的 loopback 接口，`SIOCGIFCONF` 返回 `lo` 和 `127.0.0.1`，`SIOCGIFFLAGS` 返回 `IFF_UP | IFF_LOOPBACK | IFF_RUNNING`，`SIOCSIFFLAGS` 对 `lo` 接受为成功，对未知接口返回 `ENODEV`。这种保守实现可以满足常见探测逻辑，同时避免声称存在尚未完整实现的外部网卡控制面。

```cpp
static constexpr short k_loopback_flags =
    k_iff_up | k_iff_loopback | k_iff_running;

void SocketIoctlCompat::fill_loopback_ifreq(abi::SocketIfreq &req)
{
    memset(&req, 0, sizeof(req));
    req.ifr_name[0] = 'l';
    req.ifr_name[1] = 'o';

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr = 0x0100007f;
    memcpy(&req.ifr_addr, &addr, sizeof(addr));
}
```

=== AF_UNIX 与 socketpair

除了 IPv4/IPv6 loopback，`socket_file` 还实现了 AF_UNIX 本地通信。pathname socket 在 `bind()` 时会通过 VFS 创建 `S_IFSOCK` 节点，并登记到 Unix 路径表；abstract socket 不落盘，只使用内核内 key。`socketpair()` 则绕过路径表，直接创建两个 `socket_file` 并互设 peer。

AF_UNIX stream 和 seqpacket 当前复用可靠本地 stream 队列，与 TCP loopback 使用相似的 `_peer`、`_recv_buffer`、阻塞等待和半关闭机制。这样做减少了本地可靠字节流的重复实现，也让 `socketpair()` 可以自然参与 `poll`、`epoll` 和非阻塞 I/O。

== 9.5　小结

F7LY 网络模块当前形成了清晰的“双路径”结构：loopback TCP/UDP/AF_UNIX 是已经落地并用于评测的内核内数据面，负责 localhost 场景下的真实 payload、阻塞/非阻塞、超时、信号中断和就绪通知；ONPS + VirtIO-Net 是外部 IPv4 网络后端，负责把协议栈与真实 VirtIO 网卡连接起来。

这种结构使网络模块在工程上保持了可验证性和可扩展性。可验证性来自 loopback：它绕开外部网络环境差异，直接服务于 iperf、netperf、LTP 和用户态库的核心 socket 语义。可扩展性来自 ONPS/VirtIO 适配层：设备驱动只处理帧和 virtqueue，协议栈只处理网络协议，Socket 层只处理 Linux ABI 和 fd 语义。三者边界明确，后续无论是继续增强外网 TCP/UDP，还是补充更多 ioctl、IPv6 和网卡配置能力，都可以在现有分层上推进。

= 网络系统模块

网络系统基于 Open-NPStack 协议栈实现，旨在构建高效灵活的网络通信能力。本系统支持 AF_INET、AF_INET6、AF_UNIX 及 AF_ALG 等多种地址族的套接字操作，完整实现了 TCP 与 UDP 传输协议。网络系统通过统一的抽象接口管理所有网络设备与套接字资源。

== 网络系统架构概述

F7LY-OS 的网络系统采用分层架构设计，从底层硬件驱动到上层应用接口，形成了完整的网络协议栈。整个网络系统主要包含以下几个核心组件：

+ *VirtIO 网络驱动层*：实现对虚拟网络设备的底层访问，支持 RISC-V MMIO 和 LoongArch PCI 两种接口。
+ *网络适配层*：提供对底层网络接口的统一抽象，并模拟网卡注册到网络协议栈中。
+ *网络协议协议栈*：提供完整的 TCP/IP 协议栈实现，包括以太网、IP、TCP、UDP、ICMP 等协议。
+ *BSD Socket 接口*：为用户程序提供标准的伯克利套接字接口。
+ *VFS 集成层*：将套接字抽象为文件，实现统一的文件操作接口。

#figure(
  image("fig/netarch.png", width: 85%),
  caption: [网络模块架构示意图],
) <fig:netarch>
=== 网络系统初始化流程

网络系统的初始化通过 `init_network_stack()` 函数完成，主要步骤如下：

1. *ONPS 协议栈初始化*：调用 `open_npstack_load()` 初始化网络协议栈核心
2. *VirtIO 适配器初始化*：通过 `adapter_init()` 初始化虚拟网络设备驱动
3. *网络接口注册*：将 VirtIO 网络设备注册到 ONPS 协议栈
4. *状态检查*：检查网络接口状态和 MAC 地址信息

== VirtIO 网络适配器

VirtIO 网络适配器在 F7LY-OS 网络架构中扮演关键的桥梁角色，它连接了底层的虚拟硬件设备和上层的网络协议栈。适配器的设计目标是提供高效、透明的数据传输通道，同时隐藏底层硬件的复杂性。同时，我们将这种抽象模拟成简单网卡将其注册到协议栈中。

=== 适配器设计理念

VirtIO 网络适配器采用事件驱动的异步处理模式，通过队列机制实现高效的数据传输。适配器维护独立的发送和接收线程，确保网络 I/O 操作不会阻塞系统的其他活动。

适配器实现了标准的网络接口抽象，使得上层协议栈可以透明地使用不同的底层网络设备。这种设计增强了系统的可移植性和可扩展性，为支持更多类型的网络设备奠定了基础。

== 核心网络协议栈

Open-NPStack 是 F7LY-OS 采用的第三方网络协议栈，提供了完整的 TCP/IP 协议族实现。该协议栈经过移植和优化，与内核环境深度集成。

=== 协议栈层次结构

==== 链路层 (Ethernet)

以太网层负责处理数据链路层协议：

+ *以太网帧处理*：封装和解析以太网帧头
+ *ARP 协议*：地址解析协议，实现 IP 地址到 MAC 地址的映射
+ *网络接口管理*：管理网络接口的配置和状态

```cpp
// 以太网帧结构
struct ethernet_frame {
    uint8 dst_mac[ETH_ALEN];    // 目的 MAC 地址
    uint8 src_mac[ETH_ALEN];    // 源 MAC 地址
    uint16 ethertype;           // 以太网类型
    // 负载数据...
};
```

==== 网络层 (IP)

IP 层实现了网络层协议：

+ *IPv4 协议*：支持 IPv4 数据包的路由和转发
+ *ICMP 协议*：网络控制消息协议，支持 ping 等功能
+ *路由表管理*：维护和查询路由表信息

```cpp
// IP 数据包处理流程
void ip_input(PST_NETIF pstNetif, UCHAR *pubPacket, INT nPacketLen)
{
    // 验证 IP 头部
    PST_IPHEADER pstIpHdr = (PST_IPHEADER)pubPacket;
    // 检查目的地址
    if (is_local_address(pstIpHdr->unDstAddr)) {
        // 递交给上层协议
        transport_layer_input(pstIpHdr->ubProtocol, ...);
    } else {
        // 转发数据包
        ip_forward(pstNetif, pubPacket, nPacketLen);
    }
}
```

==== 传输层 (TCP/UDP)

传输层实现了可靠和不可靠的数据传输：

+ *TCP 协议*：提供可靠的、面向连接的数据传输服务
+ *UDP 协议*：提供不可靠的、无连接的数据传输服务
+ *端口管理*：管理传输层端口分配和绑定

== BSD Socket 接口

F7LY-OS 实现了符合 POSIX 标准的 BSD Socket 接口，为用户程序提供标准的网络编程 API。Socket 接口通过 VFS 层与协议栈集成，支持文件式的操作方式。

=== Socket 文件抽象

Socket 被抽象为特殊的文件对象，集成到 VFS 中：

```cpp
class socket_file : public file
{
private:
    SocketState _state;           // Socket 状态
    SocketType _type;             // Socket 类型 (TCP/UDP)
    SocketFamily _family;         // 地址族
    SOCKET _onps_socket;          // ONPS 协议栈句柄
    struct sockaddr _local_addr;  // 本地地址
    struct sockaddr _remote_addr; // 远程地址
    
public:
    // 基本 Socket 操作
    int bind(const struct sockaddr *addr, socklen_t addrlen);
    int listen(int backlog);
    int connect(const struct sockaddr *addr, socklen_t addrlen);
    socket_file* accept(struct sockaddr *addr, socklen_t *addrlen);
    
    // 数据传输
    long send(const void *buf, size_t len, int flags);
    long recv(void *buf, size_t len, int flags);
    long sendto(const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
    long recvfrom(void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);
};
```

=== Socket 状态管理

Socket 在 F7LY-OS 中采用有限状态机进行生命周期管理，每个 Socket 对象在其生命周期内会经历多个不同的状态。这些状态反映了 Socket 的当前工作模式和可用操作，确保网络操作的正确性和安全性。

Socket 包含以下主要状态：

+ *CREATED*：Socket 刚创建完成，尚未绑定到任何地址，此时只能进行绑定操作
+ *BOUND*：Socket 已绑定到本地地址和端口，可以开始监听（TCP）或直接通信（UDP）
+ *LISTENING*：仅适用于 TCP Socket，表示正在监听来自客户端的连接请求
+ *CONNECTING*：TCP 客户端正在尝试建立连接，处于握手过程中
+ *CONNECTED*：连接已建立，可以进行双向数据传输
+ *DISCONNECTED*：连接已断开，但 Socket 对象仍然存在
+ *CLOSED*：Socket 已完全关闭，所有资源已释放

状态转换遵循严格的规则，例如只有处于 BOUND 状态的 TCP Socket 才能转换到 LISTENING 状态，只有 CONNECTED 状态的 Socket 才能进行数据传输操作。

=== 数据传输机制

F7LY-OS 的网络数据传输基于标准的 BSD Socket 接口，同时针对内核环境进行了优化。数据传输机制分为面向连接的 TCP 传输和无连接的 UDP 传输两种模式。

==== TCP 传输

TCP 数据传输提供可靠的、有序的字节流服务。当应用程序调用 `send()` 函数时，数据首先被复制到内核缓冲区，然后由 TCP 协议负责将数据分段、编号、发送，并处理确认和重传机制。TCP 的流量控制和拥塞控制算法确保数据传输的稳定性。

```cpp
long socket_file::send(const void *buf, size_t len, int flags)
{
    if (_state != SocketState::CONNECTED) {
        return -ENOTCONN;
    }
    
    int result = ::send(_onps_socket, (UCHAR*)buf, len, 5000);
    return result < 0 ? convert_onps_error() : result;
}
```

#text()[#h(2em)]对于数据接收，TCP 会将收到的数据段重新组装成有序的字节流，存储在接收缓冲区中。应用程序通过 `recv()` 函数可以按需读取数据，TCP 保证读取到的数据顺序与发送顺序完全一致。

```cpp
long socket_file::recv(void *buf, size_t len, int flags)
{
    if (_state != SocketState::CONNECTED) {
        return -ENOTCONN;
    }
    
    return ::recv(_onps_socket, (UCHAR*)buf, len);
}
```

#text()[#h(2em)]TCP 传输还支持多种选项配置，如 Nagle 算法控制、延迟确认等，以平衡网络效率和实时性需求。

==== UDP 传输

UDP 提供无连接的数据报服务，具有低延迟、高效率的特点。UDP 数据传输不需要建立连接，每个数据包都是独立的传输单元，包含完整的源地址和目的地址信息。

```cpp
long socket_file::sendto(const void *buf, size_t len, int flags,
                        const struct sockaddr *dest_addr, socklen_t addrlen)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in*)dest_addr;
    char dest_ip[16];
    inet_ntop(AF_INET, &sin->sin_addr, dest_ip, sizeof(dest_ip));
    
    return ::sendto(_onps_socket, dest_ip, ntohs(sin->sin_port),
                   (UCHAR*)buf, len);
}
```

#text()[#h(2em)]通过 `sendto()` 和 `recvfrom()` 函数，应用程序可以向任意目的地址发送数据，或从任意源地址接收数据。UDP 不提供可靠性保证，数据包可能丢失、重复或乱序到达，但这种设计使得 UDP 在实时通信、广播、多播等场景中具有独特优势。

```cpp
long socket_file::recvfrom(void *buf, size_t len, int flags,
                          struct sockaddr *src_addr, socklen_t *addrlen)
{
    void from_ip;
    USHORT from_port;
    
    int result = ::recvfrom(_onps_socket, (UCHAR*)buf, len, 
                           &from_ip, &from_port);
    fill_sockaddr_from_ip(src_addr, from_ip, from_port);
    return result;
}
```

=== 连接建立与管理

网络连接的建立和管理是网络通信的核心环节，F7LY-OS 实现了完整的 TCP 连接管理机制，支持客户端-服务器模式的网络通信。

==== 服务器端连接管理

TCP 服务器端的连接管理遵循经典的"监听-接受"模式。服务器首先通过 `bind()` 将 Socket 绑定到特定的本地地址和端口，然后调用 `listen()` 进入监听状态，指定连接队列的最大长度。

当客户端发起连接请求时，内核会将连接请求加入到待处理队列中。服务器通过 `accept()` 函数从队列中取出连接请求，完成三次握手过程，并创建新的 Socket 对象来处理该连接。原始的监听 Socket 继续保持监听状态，可以接受更多的连接请求。

这种设计允许服务器同时处理多个客户端连接，每个连接都有独立的 Socket 对象和状态管理，实现了真正的并发服务能力。

==== 客户端连接建立

TCP 客户端通过 `connect()` 函数主动发起连接建立过程。客户端指定目标服务器的 IP 地址和端口号，内核负责发送报文并等待服务器响应。连接建立过程包括三次握手的完整流程，确保双方都准备好进行数据通信。

连接建立可以是阻塞式的，客户端会等待直到连接成功或超时失败；也可以是非阻塞式的，允许客户端在连接建立过程中执行其他操作。F7LY-OS 支持两种模式，满足不同应用场景的需求。

连接建立后，客户端和服务器之间形成可靠的双向通信信道，双方可以随时发送和接收数据，直到任一方主动关闭连接或出现网络故障。


=== 数据流处理

在发送路径上，当上层协议栈需要发送网络数据包时，适配器接收数据并将其转换为 VirtIO 设备可以理解的格式。适配器负责管理发送队列、处理 DMA 映射、通知设备进行数据传输，并在传输完成后回收相关资源。

在接收路径上，适配器运行专门的接收线程，持续监听来自 VirtIO 设备的数据包。当数据包到达时，适配器立即将其提取并传递给上层协议栈进行处理。这种主动轮询的方式确保了数据包的及时处理，减少了网络延迟。

=== 系统调用接口

F7LY-OS 的网络功能通过标准的系统调用接口向用户程序提供服务，实现了用户态和内核态之间的无缝交互。系统调用接口严格遵循 POSIX 标准，确保现有网络应用程序的兼容性。

== 总结

F7LY-OS 的网络系统通过分层设计实现了完整的网络功能，从底层的 VirtIO 硬件驱动到上层的 BSD Socket 接口，形成了高效、可靠的网络通信能力。系统支持双架构硬件平台，实现了标准的 TCP/IP 协议栈，为用户程序提供了符合 POSIX 标准的网络编程接口。

网络系统的主要特点包括：
+ 模块化的分层架构设计
+ 双架构 (RISC-V/LoongArch) 硬件支持
+ 完整的 TCP/IP 协议栈实现
+ 标准的 BSD Socket 编程接口
+ 高效的零拷贝数据传输
+ 完善的错误处理和调试支持

#text()[#h(2em)]通过与 VFS 的深度集成，网络套接字能够像普通文件一样进行操作，为系统提供了统一、简洁的编程接口。整个网络系统为 F7LY-OS 的网络通信功能奠定了坚实的基础。 
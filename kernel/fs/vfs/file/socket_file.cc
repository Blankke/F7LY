#include "fs/vfs/file/socket_file.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "net/onpstack/include/onps.hh"
#include "net/onpstack/include/ip/tcp_link.hh"
#include "net/onpstack/include/ip/tcp.hh"
#include "net/onpstack/include/ip/udp.hh"
#include "net/onpstack/include/bsd/socket.hh"
#include <errno.h>
// 注意：不包含arpa/inet.h，避免冲突

namespace fs
{
    socket_file::socket_file(int domain, int type, int protocol)
        : file(FileAttrs(FT_SOCKET, 0777))
        , _state(SocketState::CREATED)
        , _type(static_cast<SocketType>(type))
        , _family(static_cast<SocketFamily>(domain))
        , _protocol(protocol)
        , _onps_socket(INVALID_SOCKET)
        , _backlog(0)
        , _blocking(true)
        , _reuse_addr(false)
    {
        new(&_stat) Kstat(FT_SOCKET);
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        _lock.init("socket_lock");
        dup();
    }

    socket_file::socket_file(FileAttrs attrs, int domain, int type, int protocol)
        : file(attrs)
        , _state(SocketState::CREATED)
        , _type(static_cast<SocketType>(type))
        , _family(static_cast<SocketFamily>(domain))
        , _protocol(protocol)
        , _onps_socket(INVALID_SOCKET)
        , _backlog(0)
        , _blocking(true)
        , _reuse_addr(false)
    {
        new(&_stat) Kstat(FT_SOCKET);
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        _lock.init("socket_lock");
        dup();
    }

    socket_file::~socket_file()
    {
        // 关闭onps socket
        if (_onps_socket != INVALID_SOCKET) {
            close(_onps_socket);
            _onps_socket = INVALID_SOCKET;
        }
        
        // 清理待处理的连接
        for (auto* pending : _pending_connections) {
            if (pending) {
                pending->free_file();
            }
        }
        _pending_connections.clear();
    }

    long socket_file::read(uint64 buf, size_t len, long off, bool upgrade)
    {
        return recv((void*)buf, len, 0);
    }

    long socket_file::write(uint64 buf, size_t len, long off, bool upgrade)
    {
        return send((const void*)buf, len, 0);
    }

    bool socket_file::read_ready()
    {
        _lock.acquire();
        bool result;
        switch (_state) {
            case SocketState::CONNECTED:
                result = !_recv_buffer.empty();
                break;
            case SocketState::LISTENING:
                result = !_pending_connections.empty();
                break;
            default:
                result = false;
                break;
        }
        _lock.release();
        return result;
    }

    bool socket_file::write_ready()
    {
        _lock.acquire();
        bool result = (_state == SocketState::CONNECTED);
        _lock.release();
        return result;
    }

    off_t socket_file::lseek(off_t offset, int whence)
    {
        // Socket不支持seek操作
        return -ESPIPE;
    }

    size_t socket_file::read_sub_dir(ubuf &dst)
    {
        // Socket不支持目录操作
        panic("socket_file::read_sub_dir: not supported");
        return 0;
    }

    int socket_file::bind(const struct sockaddr *addr, socklen_t addrlen)
    {
        if (!is_valid_address(addr, addrlen)) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (_state != SocketState::CREATED) {
            _lock.release();
            return -EINVAL;
        }

        // 复制地址信息
        int result = copy_sockaddr_from_user(&_local_addr, addr, addrlen);
        if (result < 0) {
            _lock.release();
            return -EFAULT;
        }

        // 创建onps socket句柄（如果还没有）
        if (_onps_socket == INVALID_SOCKET) {
            EN_ONPSERR onps_err;
            _onps_socket = socket(AF_INET, 
                                (_type == SocketType::TCP) ? 1 : 2, // SOCK_STREAM : SOCK_DGRAM
                                0, &onps_err);
            if (_onps_socket == INVALID_SOCKET) {
                _lock.release();
                return -ENOMEM;
            }
        }

        // 调用onps bind
        char ip_str[20];
        inet_ntoa_safe_ext(_local_addr.sin_addr, ip_str);
        
        if (::bind(_onps_socket, 
                   (_local_addr.sin_addr == 0) ? nullptr : ip_str,
                   _local_addr.sin_port) < 0) {
            close(_onps_socket);
            _onps_socket = INVALID_SOCKET;
            _lock.release();
            return -EADDRINUSE;
        }

        _state = SocketState::BOUND;
        _lock.release();
        return 0;
    }

    int socket_file::listen(int backlog)
    {
        _lock.acquire();
        
        if (_state != SocketState::BOUND) {
            _lock.release();
            return -EINVAL;
        }

        if (_type != SocketType::TCP) {
            _lock.release();
            return -EOPNOTSUPP;
        }

        _backlog = backlog > 0 ? backlog : 1;
        _state = SocketState::LISTENING;
        _pending_connections.reserve(_backlog);
        
        _lock.release();
        return 0;
    }

    socket_file* socket_file::accept(struct sockaddr *addr, socklen_t *addrlen)
    {
        _lock.acquire();
        
        if (_state != SocketState::LISTENING) {
            _lock.release();
            return nullptr;
        }

        // 检查是否有待处理的连接
        if (_pending_connections.empty()) {
            _lock.release();
            if (_blocking) {
                // 在实际实现中，这里应该等待连接
                return nullptr;
            } else {
                return nullptr;
            }
        }

        // 获取一个待处理的连接
        socket_file* client_socket = get_from_pending_queue();
        if (!client_socket) {
            _lock.release();
            return nullptr;
        }

        // 如果用户提供了地址缓冲区，复制远程地址
        if (addr && addrlen) {
            copy_sockaddr_to_user(addr, addrlen, &client_socket->_remote_addr);
        }

        client_socket->_state = SocketState::CONNECTED;
        _lock.release();
        return client_socket;
    }

    int socket_file::connect(const struct sockaddr *addr, socklen_t addrlen)
    {
        if (!is_valid_address(addr, addrlen)) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (_state != SocketState::CREATED && _state != SocketState::BOUND) {
            _lock.release();
            return -EISCONN;
        }

        // 复制远程地址
        int result = copy_sockaddr_from_user(&_remote_addr, addr, addrlen);
        if (result < 0) {
            _lock.release();
            return -EFAULT;
        }

        // 创建onps socket句柄（如果还没有）
        if (_onps_socket == INVALID_SOCKET) {
            EN_ONPSERR onps_err;
            _onps_socket = socket(AF_INET, 
                                (_type == SocketType::TCP) ? 1 : 2, // SOCK_STREAM : SOCK_DGRAM
                                0, &onps_err);
            if (_onps_socket == INVALID_SOCKET) {
                _lock.release();
                return -ENOMEM;
            }
        }

        // 进行连接
        if (_type == SocketType::TCP) {
            // TCP连接
            char ip_str[20];
            inet_ntoa_safe_ext(_remote_addr.sin_addr, ip_str);
            
            if (::connect(_onps_socket, ip_str, _remote_addr.sin_port, 5) < 0) {
                close(_onps_socket);
                _onps_socket = INVALID_SOCKET;
                _lock.release();
                return -ECONNREFUSED;
            }
        } else if (_type == SocketType::UDP) {
            // UDP "连接"（实际上是设置默认目标地址）
            // UDP socket 不需要真正的连接，只是记录远程地址
        }

        _state = SocketState::CONNECTED;
        _lock.release();
        return 0;
    }

    int socket_file::send(const void *buf, size_t len, int flags)
    {
        if (!buf || len == 0) {
            return -EINVAL;
        }

        _lock.acquire();
        
        // 检查socket是否已创建onps句柄
        if (_onps_socket == INVALID_SOCKET) {
            _lock.release();
            return -ENOTCONN;
        }

        int result = 0;
        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            // TCP send
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }

            // 使用onps TCP发送
            result = ::send(_onps_socket, const_cast<UCHAR*>(data), (INT)len, 3);
            if (result < 0) {
                _lock.release();
                return -EIO;
            }
        } else if (_type == SocketType::UDP) {
            // UDP send (需要已连接的socket)
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }

            // 使用onps UDP发送
            result = udp_send(_onps_socket, const_cast<UCHAR*>(data), (INT)len);
            if (result < 0) {
                _lock.release();
                return -EIO;
            }
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }

        _lock.release();
        return result;
    }

    int socket_file::recv(void *buf, size_t len, int flags)
    {
        if (!buf || len == 0) {
            return -EINVAL;
        }

        _lock.acquire();
        
        // 检查socket是否已创建onps句柄
        if (_onps_socket == INVALID_SOCKET) {
            _lock.release();
            return -ENOTCONN;
        }

        int result = 0;
        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            // TCP recv
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }

            // 使用onps TCP接收
            result = ::recv(_onps_socket, data, (INT)len);
            if (result < 0) {
                _lock.release();
                return -EIO;
            }
        } else if (_type == SocketType::UDP) {
            // UDP recv (不获取源地址)
            result = udp_recv_upper(_onps_socket, data, len, nullptr, nullptr, -1);
            if (result < 0) {
                _lock.release();
                return -EIO;
            }
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }

        _lock.release();
        return result;
    }

    int socket_file::sendto(const void *buf, size_t len, int flags,
                           const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        if (!buf || len == 0) {
            return -EINVAL;
        }

        _lock.acquire();

        // 检查socket是否已创建onps句柄
        if (_onps_socket == INVALID_SOCKET) {
            _lock.release();
            return -ENOTCONN;
        }

        int result = 0;
        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::UDP) {
            // UDP sendto - 需要目标地址
            if (!dest_addr || addrlen < sizeof(struct sockaddr_in)) {
                _lock.release();
                return -EINVAL;
            }

            const struct sockaddr_in* dest_addr_in = 
                reinterpret_cast<const struct sockaddr_in*>(dest_addr);
            
            if (dest_addr_in->sin_family != AF_INET) {
                _lock.release();
                return -EAFNOSUPPORT;
            }

            // 使用onps UDP sendto
            char dest_ip_str[20];
            inet_ntoa_safe_ext(dest_addr_in->sin_addr, dest_ip_str);
            
            result = ::sendto(_onps_socket, dest_ip_str, dest_addr_in->sin_port,
                            const_cast<UCHAR*>(data), (INT)len);
            
            if (result < 0) {
                _lock.release();
                return -EIO;
            }
        } else if (_type == SocketType::TCP) {
            // TCP 不支持sendto，应该使用send
            printfRed("[socket_file::sendto] TCP socket does not support sendto\n");
            _lock.release();
            return -EFAULT;
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }

        _lock.release();
        return result;
    }

    int socket_file::recvfrom(void *buf, size_t len, int flags,
                             struct sockaddr *src_addr, socklen_t *addrlen)
    {
        if (!buf || len == 0) {
            return -EINVAL;
        }

        _lock.acquire();

        // 检查socket是否已创建onps句柄
        if (_onps_socket == INVALID_SOCKET) {
            _lock.release();
            return -ENOTCONN;
        }

        int result = 0;
        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::UDP) {
            // UDP recvfrom - 获取源地址
            UINT from_ip = 0;
            USHORT from_port = 0;
            
            result = ::recvfrom(_onps_socket, data, (INT)len, &from_ip, &from_port);
            
            if (result < 0) {
                _lock.release();
                return -EIO;
            }

            // 如果用户提供了地址缓冲区，填充源地址
            if (result > 0 && src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
                struct sockaddr_in* src_addr_in = reinterpret_cast<struct sockaddr_in*>(src_addr);
                memset(src_addr_in, 0, sizeof(struct sockaddr_in));
                src_addr_in->sin_family = AF_INET;
                src_addr_in->sin_addr = from_ip;
                src_addr_in->sin_port = from_port;
                *addrlen = sizeof(struct sockaddr_in);
            }
        } else if (_type == SocketType::TCP) {
            // TCP 不支持recvfrom，应该使用recv
            _lock.release();
            return -EOPNOTSUPP;
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }

        _lock.release();
        return result;
    }

    int socket_file::shutdown(int how)
    {
        _lock.acquire();
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        // 在实际实现中，这里应该根据how参数关闭读/写/双向
        switch (how) {
            case 0: // SHUT_RD
                // 关闭读
                break;
            case 1: // SHUT_WR
                // 关闭写
                break;
            case 2: // SHUT_RDWR
                // 关闭读写
                _state = SocketState::CLOSED;
                break;
            default:
                _lock.release();
                return -EINVAL;
        }

        _lock.release();
        return 0;
    }

    int socket_file::setsockopt(int level, int optname, const void *optval, socklen_t optlen)
    {
        if (!optval) {
            return -EFAULT;
        }

        _lock.acquire();
        
        if (level == SOL_SOCKET) {
            switch (optname) {
                case SO_REUSEADDR:
                    if (optlen != sizeof(int)) {
                        _lock.release();
                        return -EINVAL;
                    }
                    _reuse_addr = *static_cast<const int*>(optval) != 0;
                    _lock.release();
                    return 0;
                    
                case SO_REUSEPORT:
                    // 暂不支持
                    _lock.release();
                    return -ENOPROTOOPT;
                    
                default:
                    _lock.release();
                    return -ENOPROTOOPT;
            }
        }
        
        _lock.release();
        return -ENOPROTOOPT;
    }

    int socket_file::getsockopt(int level, int optname, void *optval, socklen_t *optlen)
    {
        if (!optval || !optlen) {
            return -EFAULT;
        }

        _lock.acquire();
        
        if (level == SOL_SOCKET) {
            switch (optname) {
                case SO_REUSEADDR:
                    if (*optlen < sizeof(int)) {
                        _lock.release();
                        return -EINVAL;
                    }
                    *static_cast<int*>(optval) = _reuse_addr ? 1 : 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;
                    
                default:
                    _lock.release();
                    return -ENOPROTOOPT;
            }
        }
        
        _lock.release();
        return -ENOPROTOOPT;
    }

    int socket_file::getsockname(struct sockaddr *addr, socklen_t *addrlen)
    {
        if (!addr || !addrlen) {
            return -EFAULT;
        }

        _lock.acquire();
        int result = copy_sockaddr_to_user(addr, addrlen, &_local_addr);
        _lock.release();
        return result;
    }

    int socket_file::getpeername(struct sockaddr *addr, socklen_t *addrlen)
    {
        if (!addr || !addrlen) {
            return -EFAULT;
        }

        _lock.acquire();
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        // 如果是 TCP socket，从 onps 获取真实的远程地址
        if (_protocol == IPPROTO_TCP && _onps_socket >= 0) {
            // 获取输入句柄
            INT input_handle;
            EN_ONPSERR onps_err;
            if (!onps_input_get(_onps_socket, IOPT_GETATTACH, &input_handle, &onps_err)) {
                _lock.release();
                return -EINVAL;
            }
            
            // 获取 TCP 链路
            PST_TCPLINK tcp_link;
            if (!onps_input_get(input_handle, IOPT_GETTCPUDPLINK, &tcp_link, &onps_err)) {
                _lock.release();
                return -ENOTCONN;
            }
            
            if (!tcp_link) {
                _lock.release();
                return -ENOTCONN;
            }
            
            // 检查连接状态
            if (tcp_link->bState != TLSCONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            
            // 构造地址结构
            struct sockaddr_in peer_addr;
            memset(&peer_addr, 0, sizeof(peer_addr));
            peer_addr.sin_family = AF_INET;
            
#if SUPPORT_IPV6
            // IPv6 支持待添加
            _lock.release();
            return -EAFNOSUPPORT;
#else
            peer_addr.sin_addr = tcp_link->stPeer.stSockAddr.unIp;
            peer_addr.sin_port = tcp_link->stPeer.stSockAddr.usPort;
#endif
            
            // 复制到用户空间
            int result = copy_sockaddr_to_user(addr, addrlen, &peer_addr);
            _lock.release();
            return result;
        }

        // 对于非 TCP socket 或者没有 onps socket，使用存储的远程地址
        printfRed("非 TCP socket，使用存储的远程地址\n");
        int result = copy_sockaddr_to_user(addr, addrlen, &_remote_addr);
        _lock.release();
        return result;
    }

    // 私有辅助函数实现
    bool socket_file::is_valid_address(const struct sockaddr *addr, socklen_t addrlen)
    {
        if (!addr || addrlen < sizeof(struct sockaddr)) {
            return false;
        }

        if (_family == SocketFamily::INET && addrlen < sizeof(struct sockaddr_in)) {
            return false;
        }

        return true;
    }

    int socket_file::copy_sockaddr_to_user(struct sockaddr *user_addr, socklen_t *user_addrlen,
                                          const struct sockaddr_in *kernel_addr)
    {
        if (!user_addr || !user_addrlen) {
            return -EFAULT;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        socklen_t copy_len = eastl::min(*user_addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
        
        if (mem::k_vmm.copy_out(*pt, (uint64)user_addr, kernel_addr, copy_len) < 0) {
            return -EFAULT;
        }

        // 更新用户传入的地址长度
        socklen_t actual_len = sizeof(struct sockaddr_in);
        if (mem::k_vmm.copy_out(*pt, (uint64)user_addrlen, &actual_len, sizeof(socklen_t)) < 0) {
            return -EFAULT;
        }

        return 0;
    }

    int socket_file::copy_sockaddr_from_user(struct sockaddr_in *kernel_addr,
                                            const struct sockaddr *user_addr, socklen_t addrlen)
    {
        if (!kernel_addr || !user_addr) {
            return -EFAULT;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        socklen_t copy_len = eastl::min(addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
        
        if (mem::k_vmm.copy_in(*pt, kernel_addr, (uint64)user_addr, copy_len) < 0) {
            return -EFAULT;
        }

        return 0;
    }

    bool socket_file::can_accept_connection()
    {
        return _state == SocketState::LISTENING && 
               _pending_connections.size() < static_cast<size_t>(_backlog);
    }

    void socket_file::add_to_pending_queue(socket_file* client_socket)
    {
        if (can_accept_connection() && client_socket) {
            _pending_connections.push_back(client_socket);
        }
    }

    socket_file* socket_file::get_from_pending_queue()
    {
        if (_pending_connections.empty()) {
            return nullptr;
        }

        socket_file* client = _pending_connections.front();
        _pending_connections.erase(_pending_connections.begin());
        return client;
    }

    int socket_file::sendmsg(const struct msghdr *msg, int flags)
    {
        if (!msg) {
            return -EFAULT;
        }

        _lock.acquire();

        // 检查 socket 状态
        if (_state != SocketState::CONNECTED && _state != SocketState::BOUND) {
            _lock.release();
            return -ENOTCONN;
        }

        // 检查 iovec 参数
        if (!msg->msg_iov || msg->msg_iovlen == 0) {
            _lock.release();
            return -EINVAL;
        }

        // 计算总数据长度
        size_t total_len = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            if (!msg->msg_iov[i].iov_base) {
                _lock.release();
                return -EFAULT;
            }
            total_len += msg->msg_iov[i].iov_len;
        }

        if (total_len == 0) {
            _lock.release();
            return 0;
        }

        int result = 0;
        EN_ONPSERR onps_err;

        // 根据协议类型选择发送方式
        if (_protocol == IPPROTO_TCP && _onps_socket >= 0) {
            // TCP 发送：需要将所有 iovec 数据合并后发送
            eastl::vector<uint8_t> buffer;
            buffer.reserve(total_len);

            // 收集所有数据到一个缓冲区
            for (size_t i = 0; i < msg->msg_iovlen; i++) {
                const uint8_t* data = static_cast<const uint8_t*>(msg->msg_iov[i].iov_base);
                buffer.insert(buffer.end(), data, data + msg->msg_iov[i].iov_len);
            }

            // 获取输入句柄
            INT input_handle;
            if (!onps_input_get(_onps_socket, IOPT_GETATTACH, &input_handle, &onps_err)) {
                _lock.release();
                return -EINVAL;
            }

            // 使用 onps TCP 发送数据
            int sent = tcp_send_data(input_handle, buffer.data(), buffer.size(), 3);
            if (sent < 0) {
                _lock.release();
                return -EIO;
            }
            result = sent;

        } else if (_protocol == IPPROTO_UDP && _onps_socket >= 0) {
            // UDP 发送：需要目标地址
            if (!msg->msg_name || msg->msg_namelen < sizeof(struct sockaddr_in)) {
                _lock.release();
                return -EDESTADDRREQ;
            }

            struct sockaddr_in* dest_addr = static_cast<struct sockaddr_in*>(msg->msg_name);
            if (dest_addr->sin_family != AF_INET) {
                _lock.release();
                return -EAFNOSUPPORT;
            }

            // 获取输入句柄
            INT input_handle;
            if (!onps_input_get(_onps_socket, IOPT_GETATTACH, &input_handle, &onps_err)) {
                _lock.release();
                return -EINVAL;
            }

            // UDP 逐个发送每个 iovec 项（或者合并发送）
            eastl::vector<uint8_t> buffer;
            buffer.reserve(total_len);

            for (size_t i = 0; i < msg->msg_iovlen; i++) {
                const uint8_t* data = static_cast<const uint8_t*>(msg->msg_iov[i].iov_base);
                buffer.insert(buffer.end(), data, data + msg->msg_iov[i].iov_len);
            }

            // 使用 onps UDP 发送数据
            int sent = udp_sendto(input_handle, dest_addr->sin_addr, 
                                dest_addr->sin_port, buffer.data(), buffer.size());
            if (sent < 0) {
                _lock.release();
                return -EIO;
            }
            result = sent;

        } else {
            // 回退到简单实现：将数据添加到发送缓冲区
            for (size_t i = 0; i < msg->msg_iovlen; i++) {
                const uint8_t* data = static_cast<const uint8_t*>(msg->msg_iov[i].iov_base);
                size_t len = msg->msg_iov[i].iov_len;
                
                size_t old_size = _send_buffer.size();
                _send_buffer.resize(old_size + len);
                if (_send_buffer.size() != old_size + len) {
                    _send_buffer.resize(old_size);
                    _lock.release();
                    return -ENOMEM;
                }
                
                memcpy(_send_buffer.data() + old_size, data, len);
            }
            
            // 假设立即发送完成
            _send_buffer.clear();
            result = total_len;
        }

        _lock.release();
        return result;
    }

    int socket_file::recvmsg(struct msghdr *msg, int flags)
    {
        // TODO: 实现 recvmsg
        return -ENOSYS;
    }
}

#include "fs/vfs/file/socket_file.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include <errno.h>

namespace fs
{
    socket_file::socket_file(int domain, int type, int protocol)
        : file(FileAttrs(FT_SOCKET, 0777))
        , _state(SocketState::CREATED)
        , _type(static_cast<SocketType>(type))
        , _family(static_cast<SocketFamily>(domain))
        , _protocol(protocol)
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

        int result = copy_sockaddr_from_user(&_local_addr, addr, addrlen);
        if (result < 0) {
            _lock.release();
            return -EFAULT;
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

        int result = copy_sockaddr_from_user(&_remote_addr, addr, addrlen);
        if (result < 0) {
            _lock.release();
            return -EFAULT;
        }

        // 在实际实现中，这里应该进行真正的网络连接
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
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        // 简单的实现：将数据添加到发送缓冲区
        const uint8_t* data = static_cast<const uint8_t*>(buf);
        
        // 检查内存分配
        size_t old_size = _send_buffer.size();
        _send_buffer.resize(old_size + len);
        if (_send_buffer.size() != old_size + len) {
            _send_buffer.resize(old_size); // 恢复原大小
            _lock.release();
            return -ENOMEM;
        }
        
        // 复制数据
        memcpy(_send_buffer.data() + old_size, data, len);
        
        // 在实际实现中，这里应该通过网络发送数据
        _send_buffer.clear(); // 假设立即发送完成
        
        _lock.release();
        return len;
    }

    int socket_file::recv(void *buf, size_t len, int flags)
    {
        if (!buf || len == 0) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        // 检查接收缓冲区是否有数据
        if (_recv_buffer.empty()) {
            _lock.release();
            if (_blocking) {
                // 在实际实现中，这里应该等待数据到达
                return -EAGAIN;
            } else {
                return -EAGAIN;
            }
        }

        // 从接收缓冲区读取数据
        size_t copy_len = eastl::min(len, _recv_buffer.size());
        memcpy(buf, _recv_buffer.data(), copy_len);
        
        // 移除已读取的数据
        _recv_buffer.erase(_recv_buffer.begin(), _recv_buffer.begin() + copy_len);
        
        _lock.release();
        return copy_len;
    }

    int socket_file::sendto(const void *buf, size_t len, int flags,
                           const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        if (_type != SocketType::UDP) {
            return -EOPNOTSUPP;
        }

        if (!is_valid_address(dest_addr, addrlen)) {
            return -EINVAL;
        }

        // 对于UDP socket，sendto不需要连接状态
        return send(buf, len, flags);
    }

    int socket_file::recvfrom(void *buf, size_t len, int flags,
                             struct sockaddr *src_addr, socklen_t *addrlen)
    {
        if (_type != SocketType::UDP) {
            return -EOPNOTSUPP;
        }

        int result = recv(buf, len, flags);
        
        // 如果用户提供了地址缓冲区，复制源地址
        if (result > 0 && src_addr && addrlen) {
            copy_sockaddr_to_user(src_addr, addrlen, &_remote_addr);
        }
        
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
            case SHUT_RD:
                // 关闭读
                break;
            case SHUT_WR:
                // 关闭写
                break;
            case SHUT_RDWR:
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
}

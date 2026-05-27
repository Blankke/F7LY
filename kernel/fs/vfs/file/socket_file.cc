#include "fs/vfs/file/socket_file.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include <errno.h>
#include "fs/vfs/virtual_fs.hh"

namespace fs
{
    namespace
    {
        constexpr int k_loopback_binding_max = 256;
        constexpr uint32 k_loopback_addr = 0x0100007f; // 127.0.0.1 的网络字节序整数表示
        constexpr uint16 k_ephemeral_port_start = 20000;
        constexpr uint16 k_ephemeral_port_end = 60999;
        constexpr int k_protocol_ip = 0;
        constexpr int k_protocol_tcp = 6;
        constexpr int k_protocol_udp = 17;
        constexpr int k_default_socket_buffer_size = 64 * 1024;
        constexpr socklen_t k_max_user_sockaddr_len = 4096;
        constexpr int k_unix_binding_max = 256;
        constexpr int k_at_fdcwd = -100;

        struct loopback_binding
        {
            bool used = false;
            uint16 port = 0; // 始终保存 sockaddr_in.sin_port 的网络字节序值
            SocketType type = SocketType::TCP;
            socket_file *socket = nullptr;
        };

        struct unix_binding
        {
            bool used = false;
            eastl::string path;
            socket_file *socket = nullptr;
        };

        SpinLock g_loopback_lock;
        bool g_loopback_ready = false;
        uint16 g_next_ephemeral_port = k_ephemeral_port_start;
        loopback_binding g_loopback_bindings[k_loopback_binding_max];

        SpinLock g_unix_lock;
        bool g_unix_ready = false;
        unix_binding g_unix_bindings[k_unix_binding_max];

        uint16 to_network_u16(uint16 value)
        {
            return static_cast<uint16>(((value & 0x00ff) << 8) | ((value & 0xff00) >> 8));
        }

        int normalize_socket_type(int type)
        {
            return type & 0b111;
        }

        bool is_loopback_or_any(uint32 addr)
        {
            return addr == 0 || addr == k_loopback_addr || addr == 0x7f000001;
        }

        bool same_sockaddr_in(const struct sockaddr_in &lhs, const struct sockaddr_in &rhs)
        {
            return lhs.sin_family == rhs.sin_family &&
                   lhs.sin_port == rhs.sin_port &&
                   lhs.sin_addr == rhs.sin_addr;
        }

        void ensure_loopback_table()
        {
            if (!g_loopback_ready)
            {
                g_loopback_lock.init("loopback_socket_table");
                g_loopback_ready = true;
            }
        }

        void ensure_unix_table()
        {
            if (!g_unix_ready)
            {
                g_unix_lock.init("unix_socket_table");
                g_unix_ready = true;
            }
        }

        loopback_binding *find_loopback_binding(SocketType type, uint16 port)
        {
            for (auto &binding : g_loopback_bindings)
            {
                if (binding.used && binding.type == type && binding.port == port)
                {
                    return &binding;
                }
            }
            return nullptr;
        }

        int register_loopback_binding(SocketType type, uint16 port, socket_file *socket)
        {
            if (find_loopback_binding(type, port) != nullptr)
            {
                return -EADDRINUSE;
            }

            for (auto &binding : g_loopback_bindings)
            {
                if (!binding.used)
                {
                    binding.used = true;
                    binding.type = type;
                    binding.port = port;
                    binding.socket = socket;
                    return 0;
                }
            }
            return -ENFILE;
        }

        void unregister_loopback_binding(SocketType type, uint16 port, socket_file *socket)
        {
            for (auto &binding : g_loopback_bindings)
            {
                if (binding.used && binding.type == type && binding.port == port && binding.socket == socket)
                {
                    binding.used = false;
                    binding.socket = nullptr;
                    binding.port = 0;
                    return;
                }
            }
        }

        uint16 allocate_ephemeral_port(SocketType type)
        {
            for (uint32 tries = 0; tries <= (uint32)(k_ephemeral_port_end - k_ephemeral_port_start); ++tries)
            {
                uint16 host_port = g_next_ephemeral_port++;
                if (g_next_ephemeral_port > k_ephemeral_port_end)
                {
                    g_next_ephemeral_port = k_ephemeral_port_start;
                }

                uint16 net_port = to_network_u16(host_port);
                if (find_loopback_binding(type, net_port) == nullptr)
                {
                    return net_port;
                }
            }
            return 0;
        }

        eastl::string unix_path_from_sockaddr(const struct sockaddr_un &addr)
        {
            eastl::string path;
            for (size_t i = 0; i < sizeof(addr.sun_path) && addr.sun_path[i] != '\0'; ++i)
            {
                path += addr.sun_path[i];
            }
            return path;
        }

        eastl::string absolute_unix_path(const eastl::string &path)
        {
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            const char *cwd = p != nullptr ? p->_cwd_name.c_str() : "/";
            return get_absolute_path(path.c_str(), cwd);
        }

        unix_binding *find_unix_binding(const eastl::string &path)
        {
            for (auto &binding : g_unix_bindings)
            {
                if (binding.used && binding.path == path)
                {
                    return &binding;
                }
            }
            return nullptr;
        }

        int register_unix_binding(const eastl::string &path, socket_file *socket)
        {
            if (find_unix_binding(path) != nullptr)
            {
                return -EADDRINUSE;
            }

            for (auto &binding : g_unix_bindings)
            {
                if (!binding.used)
                {
                    binding.used = true;
                    binding.path = path;
                    binding.socket = socket;
                    return 0;
                }
            }
            return -ENFILE;
        }

        void unregister_unix_binding(const eastl::string &path, socket_file *socket)
        {
            for (auto &binding : g_unix_bindings)
            {
                if (binding.used && binding.path == path && binding.socket == socket)
                {
                    binding.used = false;
                    binding.path.clear();
                    binding.socket = nullptr;
                    return;
                }
            }
        }

        int unix_path_prefix_error(const eastl::string &absolute_path)
        {
            size_t slash = absolute_path.find('/', 1);
            while (slash != eastl::string::npos)
            {
                eastl::string prefix = absolute_path.substr(0, slash);
                int type = vfs_path2filetype(prefix);
                if (type >= 0 && type != fs::FileTypes::FT_DIRECT)
                {
                    return -ENOTDIR;
                }
                if (type < 0)
                {
                    return -ENOENT;
                }
                slash = absolute_path.find('/', slash + 1);
            }
            return 0;
        }
    }

    socket_file::socket_file(int domain, int type, int protocol)
        : file(FileAttrs(FT_SOCKET, 0777))
        , _state(SocketState::CREATED)
        , _type(static_cast<SocketType>(normalize_socket_type(type)))
        , _family(static_cast<SocketFamily>(domain))
        , _protocol(protocol)
        , _onps_socket(INVALID_SOCKET)
        , _backlog(0)
        , _peer(nullptr)
        , _blocking(true)
        , _reuse_addr(false)
        , _loopback_registered(false)
        , _unix_registered(false)
        , _read_shutdown(false)
        , _write_shutdown(false)
        , _peer_closed(false)
        , _pending_send_has_addr(false)
    {
        new(&_stat) Kstat(FT_SOCKET);
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        memset(&_pending_send_addr, 0, sizeof(_pending_send_addr));
        memset(&_local_unix_addr, 0, sizeof(_local_unix_addr));
        memset(&_remote_unix_addr, 0, sizeof(_remote_unix_addr));
        lwext4_file_struct.flags = O_RDWR;
        _lock.init("socket_lock");
        dup();
    }

    socket_file::socket_file(FileAttrs attrs, int domain, int type, int protocol)
        : file(attrs)
        , _state(SocketState::CREATED)
        , _type(static_cast<SocketType>(normalize_socket_type(type)))
        , _family(static_cast<SocketFamily>(domain))
        , _protocol(protocol)
        , _onps_socket(INVALID_SOCKET)
        , _backlog(0)
        , _peer(nullptr)
        , _blocking(true)
        , _reuse_addr(false)
        , _loopback_registered(false)
        , _unix_registered(false)
        , _read_shutdown(false)
        , _write_shutdown(false)
        , _peer_closed(false)
    {
        new(&_stat) Kstat(FT_SOCKET);
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        memset(&_local_unix_addr, 0, sizeof(_local_unix_addr));
        memset(&_remote_unix_addr, 0, sizeof(_remote_unix_addr));
        lwext4_file_struct.flags = O_RDWR;
        _lock.init("socket_lock");
        dup();
    }

    socket_file::~socket_file()
    {
        _lock.acquire();
        _state = SocketState::CLOSED;
        _send_buffer.clear();
        _pending_send_has_addr = false;
        proc::k_pm.wakeup(&_pending_connections);
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
        _lock.release();

        ensure_loopback_table();
        if (_loopback_registered)
        {
            g_loopback_lock.acquire();
            unregister_loopback_binding(_type, _local_addr.sin_port, this);
            g_loopback_lock.release();
            _loopback_registered = false;
        }

        ensure_unix_table();
        if (_unix_registered)
        {
            g_unix_lock.acquire();
            unregister_unix_binding(_unix_path, this);
            g_unix_lock.release();
            _unix_registered = false;
        }

        // fd 关闭时通知对端，阻塞在 recv/accept 的进程需要被唤醒。
        if (_peer != nullptr)
        {
            _peer->_lock.acquire();
            if (_peer->_peer == this)
            {
                _peer->_peer = nullptr;
            }
            _peer->_peer_closed = true;
            proc::k_pm.wakeup(&_peer->_recv_buffer);
            proc::k_pm.wakeup(&_peer->_datagram_queue);
            _peer->_lock.release();
            _peer = nullptr;
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
                if (_type == SocketType::UDP)
                {
                    result = !_datagram_queue.empty();
                }
                else
                {
                    result = !_recv_buffer.empty() || _peer_closed || _read_shutdown;
                }
                break;
            case SocketState::LISTENING:
                result = !_pending_connections.empty();
                break;
            case SocketState::BOUND:
                result = _type == SocketType::UDP && !_datagram_queue.empty();
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
        bool result = false;
        if (_state == SocketState::CONNECTED)
        {
            result = !_write_shutdown && !_peer_closed && (_type == SocketType::UDP || _peer != nullptr);
        }
        else if (_type == SocketType::UDP && (_state == SocketState::CREATED || _state == SocketState::BOUND))
        {
            result = !_write_shutdown;
        }
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

        if (_family != SocketFamily::INET) {
            if (_family != SocketFamily::UNIX) {
                _lock.release();
                return -EAFNOSUPPORT;
            }
            if (_type != SocketType::TCP) {
                _lock.release();
                return -EOPNOTSUPP;
            }
            if (addrlen < sizeof(struct sockaddr_un)) {
                _lock.release();
                return -EINVAL;
            }

            struct sockaddr_un local_unix_addr;
            memcpy(&local_unix_addr, addr, sizeof(local_unix_addr));
            if (local_unix_addr.sun_family != AF_UNIX) {
                _lock.release();
                return -EAFNOSUPPORT;
            }

            eastl::string relative_path = unix_path_from_sockaddr(local_unix_addr);
            if (relative_path.empty()) {
                _lock.release();
                return -EINVAL;
            }
            eastl::string absolute_path = absolute_unix_path(relative_path);
            _lock.release();

            int prefix_error = unix_path_prefix_error(absolute_path);
            if (prefix_error < 0) {
                return prefix_error;
            }

            // pathname AF_UNIX bind 在文件系统中有可 unlink 的 socket 节点；
            // recvmsg01 的 cleanup 依赖这个节点真实存在。
            int node_result = proc::k_pm.mknod(k_at_fdcwd, relative_path, S_IFSOCK | 0777, 0);
            if (node_result < 0) {
                return node_result == -EEXIST ? -EADDRINUSE : node_result;
            }

            ensure_unix_table();
            g_unix_lock.acquire();
            int register_result = register_unix_binding(absolute_path, this);
            g_unix_lock.release();
            if (register_result < 0) {
                proc::k_pm.unlink(k_at_fdcwd, relative_path, 0);
                return register_result;
            }

            _lock.acquire();
            if (_state != SocketState::CREATED) {
                _lock.release();
                g_unix_lock.acquire();
                unregister_unix_binding(absolute_path, this);
                g_unix_lock.release();
                proc::k_pm.unlink(k_at_fdcwd, relative_path, 0);
                return -EINVAL;
            }
            _local_unix_addr = local_unix_addr;
            _unix_path = absolute_path;
            _unix_registered = true;
            _state = SocketState::BOUND;
            _lock.release();
            return 0;
        }

        struct sockaddr_in local_addr;
        memcpy(&local_addr, addr, sizeof(local_addr));
        if (local_addr.sin_family != AF_INET) {
            _lock.release();
            return -EAFNOSUPPORT;
        }
        if (!is_loopback_or_any(local_addr.sin_addr)) {
            _lock.release();
            return -EADDRNOTAVAIL;
        }

        ensure_loopback_table();
        g_loopback_lock.acquire();

        if (local_addr.sin_port == 0) {
            local_addr.sin_port = allocate_ephemeral_port(_type);
            if (local_addr.sin_port == 0) {
                g_loopback_lock.release();
                _lock.release();
                return -EADDRINUSE;
            }
        }

        int result = register_loopback_binding(_type, local_addr.sin_port, this);
        if (result < 0) {
            g_loopback_lock.release();
            _lock.release();
            return result;
        }

        _local_addr = local_addr;
        _loopback_registered = true;
        _state = SocketState::BOUND;
        g_loopback_lock.release();
        _lock.release();
        return 0;
    }

    int socket_file::listen(int backlog)
    {
        _lock.acquire();
        
        if (_type != SocketType::TCP) {
            _lock.release();
            return -EOPNOTSUPP;
        }

        if (_state == SocketState::CREATED) {
            int bind_result = ensure_loopback_bound_locked();
            if (bind_result < 0) {
                _lock.release();
                return bind_result;
            }
        }

        if (_state != SocketState::BOUND) {
            _lock.release();
            return -EINVAL;
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
        while (_pending_connections.empty()) {
            if (!_blocking) {
                _lock.release();
                return nullptr;
            }
            proc::k_pm.sleep(&_pending_connections, &_lock);
            if (_state != SocketState::LISTENING) {
                _lock.release();
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
            if (_family == SocketFamily::UNIX) {
                socklen_t copy_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_un)));
                memcpy(addr, &client_socket->_remote_unix_addr, copy_len);
                *addrlen = sizeof(struct sockaddr_un);
            } else {
                socklen_t copy_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                memcpy(addr, &client_socket->_remote_addr, copy_len);
                *addrlen = sizeof(struct sockaddr_in);
            }
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
        // 根据 socket 族类型处理不同的连接方式
        if (_family == SocketFamily::UNIX) {
            if (_type != SocketType::TCP) {
                _lock.release();
                return -EOPNOTSUPP;
            }
            if (addrlen < sizeof(struct sockaddr_un)) {
                _lock.release();
                return -EINVAL;
            }

            struct sockaddr_un remote_unix_addr;
            memcpy(&remote_unix_addr, addr, sizeof(remote_unix_addr));
            if (remote_unix_addr.sun_family != AF_UNIX) {
                _lock.release();
                return -EAFNOSUPPORT;
            }

            eastl::string relative_path = unix_path_from_sockaddr(remote_unix_addr);
            if (relative_path.empty()) {
                _lock.release();
                return -EINVAL;
            }
            eastl::string absolute_path = absolute_unix_path(relative_path);

            ensure_unix_table();
            g_unix_lock.acquire();
            unix_binding *binding = find_unix_binding(absolute_path);
            socket_file *listener = binding ? binding->socket : nullptr;
            if (listener == nullptr) {
                g_unix_lock.release();
                _lock.release();
                return -ENOENT;
            }

            listener->_lock.acquire();
            if (listener->_state != SocketState::LISTENING || !listener->can_accept_connection()) {
                listener->_lock.release();
                g_unix_lock.release();
                _lock.release();
                return -ECONNREFUSED;
            }

            socket_file *server_side = new socket_file(AF_UNIX, SOCK_STREAM, _protocol);
            if (server_side == nullptr) {
                listener->_lock.release();
                g_unix_lock.release();
                _lock.release();
                return -ENOMEM;
            }

            memset(&_local_unix_addr, 0, sizeof(_local_unix_addr));
            _local_unix_addr.sun_family = AF_UNIX;
            _remote_unix_addr = remote_unix_addr;
            server_side->_local_unix_addr = listener->_local_unix_addr;
            server_side->_remote_unix_addr = _local_unix_addr;
            server_side->_state = SocketState::CONNECTED;
            server_side->_peer = this;

            _peer = server_side;
            _peer_closed = false;
            _state = SocketState::CONNECTED;
            listener->add_to_pending_queue(server_side);
            proc::k_pm.wakeup(&listener->_pending_connections);
            listener->_lock.release();
            g_unix_lock.release();
            _lock.release();
            return 0;

        } else if (_family == SocketFamily::INET) {
            struct sockaddr_in remote_addr;
            memcpy(&remote_addr, addr, sizeof(remote_addr));
            if (remote_addr.sin_family != AF_INET) {
                _lock.release();
                return -EAFNOSUPPORT;
            }
            if (!is_loopback_or_any(remote_addr.sin_addr)) {
                _lock.release();
                return -ENETUNREACH;
            }

            if (_state == SocketState::CREATED) {
                int bind_result = ensure_loopback_bound_locked();
                if (bind_result < 0) {
                    _lock.release();
                    return bind_result;
                }
            }

            _remote_addr = remote_addr;

            if (_type == SocketType::UDP) {
                _state = SocketState::CONNECTED;
                _lock.release();
                return 0;
            }

            if (_type != SocketType::TCP) {
                _lock.release();
                return -EOPNOTSUPP;
            }

            ensure_loopback_table();
            g_loopback_lock.acquire();
            loopback_binding *binding = find_loopback_binding(SocketType::TCP, remote_addr.sin_port);
            socket_file *listener = binding ? binding->socket : nullptr;
            if (listener == nullptr) {
                g_loopback_lock.release();
                _lock.release();
                return -ECONNREFUSED;
            }

            listener->_lock.acquire();
            if (listener->_state != SocketState::LISTENING || !listener->can_accept_connection()) {
                listener->_lock.release();
                g_loopback_lock.release();
                _lock.release();
                return -ECONNREFUSED;
            }

            socket_file *server_side = new socket_file(AF_INET, SOCK_STREAM, _protocol);
            if (server_side == nullptr) {
                listener->_lock.release();
                g_loopback_lock.release();
                _lock.release();
                return -ENOMEM;
            }

            if (_local_addr.sin_addr == 0) {
                _local_addr.sin_addr = k_loopback_addr;
            }
            server_side->_local_addr = listener->_local_addr;
            if (server_side->_local_addr.sin_addr == 0) {
                server_side->_local_addr.sin_addr = k_loopback_addr;
            }
            server_side->_remote_addr = _local_addr;
            server_side->_state = SocketState::CONNECTED;
            server_side->_peer = this;

            _peer = server_side;
            _peer_closed = false;
            _state = SocketState::CONNECTED;
            listener->add_to_pending_queue(server_side);
            proc::k_pm.wakeup(&listener->_pending_connections);
            listener->_lock.release();
            g_loopback_lock.release();
            _lock.release();
            return 0;

        } else {
            // 不支持的协议族
            _lock.release();
            return -EAFNOSUPPORT;
        }
    }

    int socket_file::send(const void *buf, size_t len, int flags)
    {
        if (!buf) {
            return -EFAULT;
        }
        if (len == 0) {
            return 0;
        }

        _lock.acquire();

        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            if (_write_shutdown || _peer == nullptr || _peer_closed) {
                _lock.release();
                return -EPIPE;
            }
            socket_file *peer = _peer;
            eastl::vector<uint8_t> flush_buffer;
            const uint8_t *send_data = data;
            size_t send_len = len;

            if (flags & MSG_MORE) {
                int append_result = append_pending_send_locked(data, len, nullptr);
                _lock.release();
                return append_result;
            }

            if (!_send_buffer.empty()) {
                flush_buffer.reserve(_send_buffer.size() + len);
                flush_buffer.insert(flush_buffer.end(), _send_buffer.begin(), _send_buffer.end());
                flush_buffer.insert(flush_buffer.end(), data, data + len);
                _send_buffer.clear();
                _pending_send_has_addr = false;
                send_data = flush_buffer.data();
                send_len = flush_buffer.size();
            }
            _lock.release();

            // 只在本端锁内读取连接状态；实际入队时只持有对端锁，避免双向 send 互相等待。
            peer->_lock.acquire();
            if (peer->_read_shutdown || peer->_state == SocketState::CLOSED)
            {
                peer->_lock.release();
                return -EPIPE;
            }
            size_t old_size = peer->_recv_buffer.size();
            peer->_recv_buffer.resize(old_size + send_len);
            memcpy(peer->_recv_buffer.data() + old_size, send_data, send_len);
            proc::k_pm.wakeup(&peer->_recv_buffer);
            peer->_lock.release();
            return static_cast<int>(len);
        } else if (_type == SocketType::UDP) {
            if (_write_shutdown) {
                _lock.release();
                return -EPIPE;
            }
            if (_peer != nullptr) {
                struct sockaddr_in src = _local_addr;
                if (src.sin_addr == 0) {
                    src.sin_addr = k_loopback_addr;
                }
                socket_file *peer = _peer;
                _lock.release();

                peer->_lock.acquire();
                int result = peer->enqueue_datagram(&src, data, len);
                peer->_lock.release();
                return result;
            }
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -EDESTADDRREQ;
            }
            struct sockaddr_in remote = _remote_addr;
            _lock.release();
            return sendto(buf, len, flags, reinterpret_cast<const struct sockaddr *>(&remote), sizeof(remote));
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }
    }

    int socket_file::recv(void *buf, size_t len, int flags)
    {
        if (!buf) {
            return -EFAULT;
        }
        if (len == 0) {
            return 0;
        }

        _lock.acquire();

        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }

            while (_recv_buffer.empty() && !_peer_closed && !_read_shutdown) {
                if (is_nonblocking_request(flags)) {
                    _lock.release();
                    return -EAGAIN;
                }
                proc::k_pm.sleep(&_recv_buffer, &_lock);
            }

            if (_recv_buffer.empty()) {
                _lock.release();
                return 0;
            }

            size_t copy_len = eastl::min(len, _recv_buffer.size());
            memcpy(data, _recv_buffer.data(), copy_len);
            if (!(flags & MSG_PEEK)) {
                _recv_buffer.erase(_recv_buffer.begin(), _recv_buffer.begin() + copy_len);
            }
            _lock.release();
            return static_cast<int>(copy_len);
        } else if (_type == SocketType::UDP) {
            _lock.release();
            int result = recvfrom(buf, len, flags, nullptr, nullptr);
            return result;
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }
    }

    int socket_file::sendto(const void *buf, size_t len, int flags,
                           const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        if (!buf) {
            return -EFAULT;
        }
        if (len == 0) {
            return 0;
        }
        if (flags & MSG_OOB) {
            return _type == SocketType::UDP ? -EOPNOTSUPP : -EINVAL;
        }

        _lock.acquire();

        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::UDP) {
            if (len > k_default_socket_buffer_size) {
                _lock.release();
                return -EMSGSIZE;
            }
            struct sockaddr_in dest;
            if (dest_addr != nullptr) {
                if (addrlen < sizeof(struct sockaddr_in)) {
                    _lock.release();
                    return -EINVAL;
                }
                memcpy(&dest, dest_addr, sizeof(dest));
                if (dest.sin_family != AF_INET) {
                    _lock.release();
                    return -EAFNOSUPPORT;
                }
            } else {
                if (_state != SocketState::CONNECTED) {
                    _lock.release();
                    return -EDESTADDRREQ;
                }
                dest = _remote_addr;
            }

            if (!is_loopback_or_any(dest.sin_addr)) {
                _lock.release();
                return -ENETUNREACH;
            }

            if (_state == SocketState::CREATED) {
                int bind_result = ensure_loopback_bound_locked();
                if (bind_result < 0) {
                    _lock.release();
                    return bind_result;
                }
            }

            struct sockaddr_in src = _local_addr;
            if (src.sin_addr == 0) {
                src.sin_addr = k_loopback_addr;
            }
            eastl::vector<uint8_t> flush_buffer;
            const uint8_t *send_data = data;
            size_t send_len = len;
            struct sockaddr_in send_dest = dest;

            if (flags & MSG_MORE) {
                int append_result = append_pending_send_locked(data, len, &dest);
                _lock.release();
                return append_result;
            }

            if (!_send_buffer.empty()) {
                if (!pending_send_destination_matches_locked(&dest)) {
                    _lock.release();
                    return -EINVAL;
                }
                if (_send_buffer.size() + len > static_cast<size_t>(k_default_socket_buffer_size)) {
                    _lock.release();
                    return -EMSGSIZE;
                }
                send_dest = _pending_send_addr;
                flush_buffer.reserve(_send_buffer.size() + len);
                flush_buffer.insert(flush_buffer.end(), _send_buffer.begin(), _send_buffer.end());
                flush_buffer.insert(flush_buffer.end(), data, data + len);
                _send_buffer.clear();
                _pending_send_has_addr = false;
                send_data = flush_buffer.data();
                send_len = flush_buffer.size();
            }

            _lock.release();

            socket_file *target = nullptr;
            g_loopback_lock.acquire();
            loopback_binding *binding = find_loopback_binding(SocketType::UDP, send_dest.sin_port);
            target = binding ? binding->socket : nullptr;
            g_loopback_lock.release();

            if (target != nullptr) {
                target->_lock.acquire();
                target->enqueue_datagram(&src, send_data, send_len);
                target->_lock.release();
            }
            return static_cast<int>(len);
        } else if (_type == SocketType::TCP) {
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -EPIPE;
            }
            _lock.release();
            return send(buf, len, flags);
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }
    }

    int socket_file::recvfrom(void *buf, size_t len, int flags,
                             struct sockaddr *src_addr, socklen_t *addrlen)
    {
        if (!buf) {
            return -EFAULT;
        }
        if (len == 0) {
            return 0;
        }
        if (flags & MSG_OOB) {
            return -EINVAL;
        }
        if (flags & MSG_ERRQUEUE) {
            return -EAGAIN;
        }

        _lock.acquire();

        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::UDP) {
            if (_state != SocketState::BOUND && _state != SocketState::CONNECTED) {
                _lock.release();
                return -EINVAL;
            }

            while (_datagram_queue.empty() && !_read_shutdown) {
                if (is_nonblocking_request(flags)) {
                    _lock.release();
                    return -EAGAIN;
                }
                proc::k_pm.sleep(&_datagram_queue, &_lock);
            }

            if (_datagram_queue.empty()) {
                _lock.release();
                return 0;
            }

            loopback_datagram &packet = _datagram_queue.front();
            size_t copy_len = eastl::min(len, packet.data.size());
            memcpy(data, packet.data.data(), copy_len);

            if (src_addr && addrlen && *addrlen > 0) {
                socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                memcpy(src_addr, &packet.src_addr, copy_addr_len);
                *addrlen = sizeof(struct sockaddr_in);
            }

            if (!(flags & MSG_PEEK)) {
                _datagram_queue.erase(_datagram_queue.begin());
            }
            _lock.release();
            return static_cast<int>(copy_len);
        } else if (_type == SocketType::TCP) {
            struct sockaddr_in peer_addr = _remote_addr;
            struct sockaddr_un peer_unix_addr = _remote_unix_addr;
            bool unix_socket = _family == SocketFamily::UNIX;
            _lock.release();
            int result = recv(buf, len, flags);
            if (result >= 0 && src_addr && addrlen && *addrlen > 0) {
                if (unix_socket) {
                    socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_un)));
                    memcpy(src_addr, &peer_unix_addr, copy_addr_len);
                    *addrlen = sizeof(struct sockaddr_un);
                } else {
                    socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                    memcpy(src_addr, &peer_addr, copy_addr_len);
                    *addrlen = sizeof(struct sockaddr_in);
                }
            }
            return result;
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }
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
                _read_shutdown = true;
                _recv_buffer.clear();
                _datagram_queue.clear();
                break;
            case 1: // SHUT_WR
                _write_shutdown = true;
                _send_buffer.clear();
                _pending_send_has_addr = false;
                break;
            case 2: // SHUT_RDWR
                _read_shutdown = true;
                _write_shutdown = true;
                _recv_buffer.clear();
                _send_buffer.clear();
                _datagram_queue.clear();
                _pending_send_has_addr = false;
                _state = SocketState::CLOSED;
                break;
            default:
                _lock.release();
                return -EINVAL;
        }

        socket_file *peer = nullptr;
        if ((how == SHUT_WR || how == SHUT_RDWR) && _peer != nullptr) {
            peer = _peer;
        }

        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
        _lock.release();

        if (peer != nullptr) {
            peer->_lock.acquire();
            peer->_peer_closed = true;
            proc::k_pm.wakeup(&peer->_recv_buffer);
            proc::k_pm.wakeup(&peer->_datagram_queue);
            peer->_lock.release();
        }
        return 0;
    }

    int socket_file::setsockopt(int level, int optname, const void *optval, socklen_t optlen)
    {
        if (!optval) {
            return -EFAULT;
        }
        if (optlen == 0) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (level == SOL_SOCKET) {
            switch (optname) {
                case SO_REUSEADDR:
                case SO_REUSEPORT:
                    if (optlen != sizeof(int)) {
                        _lock.release();
                        return -EINVAL;
                    }
                    _reuse_addr = *static_cast<const int*>(optval) != 0;
                    _lock.release();
                    return 0;

                case SO_KEEPALIVE:
                case SO_BROADCAST:
                case SO_OOBINLINE:
                case SO_SNDBUF:
                case SO_RCVBUF:
                    // loopback 初版没有真实网卡缓存和带外数据；常见调优项接受为 no-op，
                    // 避免 iperf/netperf 在初始化阶段因非核心选项失败而退出。
                    if (optlen < sizeof(int)) {
                        _lock.release();
                        return -EINVAL;
                    }
                    _lock.release();
                    return 0;

                default:
                    _lock.release();
                    return -ENOPROTOOPT;
            }
        }
        else if (level == k_protocol_tcp) {
            // TCP_NODELAY 等 TCP 调优项在内核 loopback 中没有 Nagle/拥塞控制语义，先稳定 no-op。
            if (_type != SocketType::TCP || optname < 0 || optlen < sizeof(int)) {
                _lock.release();
                return -ENOPROTOOPT;
            }
            _lock.release();
            return 0;
        }
        else if (level == k_protocol_ip || level == k_protocol_udp) {
            _lock.release();
            return -ENOPROTOOPT;
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
            if (*optlen < sizeof(int)) {
                _lock.release();
                return -EINVAL;
            }

            switch (optname) {
                case SO_REUSEADDR:
                    *static_cast<int*>(optval) = _reuse_addr ? 1 : 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_TYPE:
                    *static_cast<int*>(optval) = static_cast<int>(_type);
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_ERROR:
                    *static_cast<int*>(optval) = 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_ACCEPTCONN:
                    *static_cast<int*>(optval) = _state == SocketState::LISTENING ? 1 : 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_SNDBUF:
                case SO_RCVBUF:
                    *static_cast<int*>(optval) = k_default_socket_buffer_size;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_KEEPALIVE:
                case SO_BROADCAST:
                case SO_OOBINLINE:
                case SO_REUSEPORT:
                    *static_cast<int*>(optval) = 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_PROTOCOL:
                    *static_cast<int*>(optval) = _protocol;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_DOMAIN:
                    *static_cast<int*>(optval) = static_cast<int>(_family);
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                default:
                    _lock.release();
                    return -ENOPROTOOPT;
            }
        }
        else if (level == k_protocol_tcp || level == k_protocol_ip) {
            _lock.release();
            return -ENOPROTOOPT;
        }
        else if (level == k_protocol_udp) {
            _lock.release();
            return -EOPNOTSUPP;
        }
        
        _lock.release();
        return -EOPNOTSUPP;
    }

    int socket_file::getsockname(struct sockaddr *addr, socklen_t *addrlen)
    {
        if (!addr || !addrlen) {
            return -EFAULT;
        }
        if ((uint64)addr < sizeof(struct sockaddr) || (uint64)addrlen < sizeof(socklen_t)) {
            return -EFAULT;
        }

        _lock.acquire();
        if (_family == SocketFamily::UNIX) {
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            mem::PageTable *pt = p->get_pagetable();
            socklen_t requested_len = 0;
            if (mem::k_vmm.copy_in(*pt, &requested_len, (uint64)addrlen, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            if (requested_len > k_max_user_sockaddr_len) {
                _lock.release();
                return -EINVAL;
            }
            socklen_t copy_len = eastl::min(requested_len, static_cast<socklen_t>(sizeof(struct sockaddr_un)));
            if (mem::k_vmm.copy_out(*pt, (uint64)addr, &_local_unix_addr, copy_len) < 0) {
                _lock.release();
                return -EFAULT;
            }
            socklen_t actual_len = sizeof(struct sockaddr_un);
            if (mem::k_vmm.copy_out(*pt, (uint64)addrlen, &actual_len, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            _lock.release();
            return 0;
        }
        int result = copy_sockaddr_to_user(addr, addrlen, &_local_addr);
        _lock.release();
        return result;
    }

    int socket_file::getpeername(struct sockaddr *addr, socklen_t *addrlen)
    {
        if (!addr || !addrlen) {
            return -EFAULT;
        }
        if ((uint64)addr < sizeof(struct sockaddr) || (uint64)addrlen < sizeof(socklen_t)) {
            return -EFAULT;
        }

        _lock.acquire();
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        if (_family == SocketFamily::UNIX) {
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            mem::PageTable *pt = p->get_pagetable();
            socklen_t requested_len = 0;
            if (mem::k_vmm.copy_in(*pt, &requested_len, (uint64)addrlen, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            if (requested_len > k_max_user_sockaddr_len) {
                _lock.release();
                return -EINVAL;
            }
            socklen_t copy_len = eastl::min(requested_len, static_cast<socklen_t>(sizeof(struct sockaddr_un)));
            if (mem::k_vmm.copy_out(*pt, (uint64)addr, &_remote_unix_addr, copy_len) < 0) {
                _lock.release();
                return -EFAULT;
            }
            socklen_t actual_len = sizeof(struct sockaddr_un);
            if (mem::k_vmm.copy_out(*pt, (uint64)addrlen, &actual_len, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            _lock.release();
            return 0;
        }

        int result = copy_sockaddr_to_user(addr, addrlen, &_remote_addr);
        _lock.release();
        return result;
    }

    // 私有辅助函数实现
    bool socket_file::is_nonblocking_request(int flags) const
    {
        return !_blocking || (flags & MSG_DONTWAIT);
    }

    int socket_file::append_pending_send_locked(const uint8_t *data, size_t len,
                                                const struct sockaddr_in *dest_addr)
    {
        if (data == nullptr) {
            return -EFAULT;
        }
        if (_type == SocketType::UDP && dest_addr != nullptr) {
            if (!_pending_send_has_addr) {
                _pending_send_addr = *dest_addr;
                _pending_send_has_addr = true;
            } else if (!pending_send_destination_matches_locked(dest_addr)) {
                return -EINVAL;
            }
        }
        if (_send_buffer.size() + len > static_cast<size_t>(k_default_socket_buffer_size)) {
            return -EMSGSIZE;
        }

        size_t old_size = _send_buffer.size();
        _send_buffer.resize(old_size + len);
        memcpy(_send_buffer.data() + old_size, data, len);
        return static_cast<int>(len);
    }

    bool socket_file::pending_send_destination_matches_locked(const struct sockaddr_in *dest_addr) const
    {
        if (!_pending_send_has_addr || dest_addr == nullptr) {
            return true;
        }
        return same_sockaddr_in(_pending_send_addr, *dest_addr);
    }

    int socket_file::ensure_loopback_bound_locked()
    {
        if (_loopback_registered)
        {
            return 0;
        }
        if (_family != SocketFamily::INET)
        {
            return -EAFNOSUPPORT;
        }

        ensure_loopback_table();
        g_loopback_lock.acquire();
        uint16 port = allocate_ephemeral_port(_type);
        if (port == 0)
        {
            g_loopback_lock.release();
            return -EADDRINUSE;
        }

        int result = register_loopback_binding(_type, port, this);
        if (result < 0)
        {
            g_loopback_lock.release();
            return result;
        }

        memset(&_local_addr, 0, sizeof(_local_addr));
        _local_addr.sin_family = AF_INET;
        _local_addr.sin_addr = k_loopback_addr;
        _local_addr.sin_port = port;
        _loopback_registered = true;
        _state = SocketState::BOUND;
        g_loopback_lock.release();
        return 0;
    }

    int socket_file::enqueue_stream_data(const uint8_t *data, size_t len)
    {
        if (_peer == nullptr || _peer_closed)
        {
            return -EPIPE;
        }

        socket_file *peer = _peer;
        peer->_lock.acquire();
        if (peer->_read_shutdown || peer->_state == SocketState::CLOSED)
        {
            peer->_lock.release();
            return -EPIPE;
        }

        size_t old_size = peer->_recv_buffer.size();
        peer->_recv_buffer.resize(old_size + len);
        memcpy(peer->_recv_buffer.data() + old_size, data, len);
        proc::k_pm.wakeup(&peer->_recv_buffer);
        peer->_lock.release();
        return static_cast<int>(len);
    }

    int socket_file::enqueue_datagram(const struct sockaddr_in *src_addr, const uint8_t *data, size_t len)
    {
        if (_read_shutdown || _state == SocketState::CLOSED)
        {
            return -EPIPE;
        }

        loopback_datagram packet;
        memset(&packet.src_addr, 0, sizeof(packet.src_addr));
        if (src_addr != nullptr)
        {
            packet.src_addr = *src_addr;
        }
        packet.data.resize(len);
        if (len > 0)
        {
            memcpy(packet.data.data(), data, len);
        }
        _datagram_queue.push_back(packet);
        proc::k_pm.wakeup(&_datagram_queue);
        return static_cast<int>(len);
    }

    void socket_file::attach_loopback_peer(socket_file *peer)
    {
        _lock.acquire();
        _peer = peer;
        _peer_closed = peer == nullptr;
        _state = peer == nullptr ? SocketState::CLOSED : SocketState::CONNECTED;
        _lock.release();
    }

    bool socket_file::is_valid_address(const struct sockaddr *addr, socklen_t addrlen)
    {
        if (!addr || addrlen < sizeof(struct sockaddr)) {
            return false;
        }

        if (_family == SocketFamily::INET && addrlen < sizeof(struct sockaddr_in)) {
            return false;
        }

        if (_family == SocketFamily::UNIX && addrlen < sizeof(struct sockaddr_un)) {
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
        if ((uint64)user_addr < sizeof(struct sockaddr) || (uint64)user_addrlen < sizeof(socklen_t)) {
            return -EFAULT;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        mem::PageTable *pt = p->get_pagetable();

        socklen_t requested_len = 0;
        if (mem::k_vmm.copy_in(*pt, &requested_len, (uint64)user_addrlen, sizeof(socklen_t)) < 0) {
            return -EFAULT;
        }
        if (requested_len > k_max_user_sockaddr_len) {
            return -EINVAL;
        }

        socklen_t copy_len = eastl::min(requested_len, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
        
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

        // 检查 iovec 参数
        if (!msg->msg_iov || msg->msg_iovlen == 0) {
            return -EINVAL;
        }

        // 计算总数据长度
        size_t total_len = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            if (!msg->msg_iov[i].iov_base) {
                return -EFAULT;
            }
            total_len += msg->msg_iov[i].iov_len;
        }

        if (total_len == 0) {
            return 0;
        }

        eastl::vector<uint8_t> buffer;
        buffer.reserve(total_len);
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            const uint8_t* data = static_cast<const uint8_t*>(msg->msg_iov[i].iov_base);
            buffer.insert(buffer.end(), data, data + msg->msg_iov[i].iov_len);
        }

        if (msg->msg_name != nullptr && msg->msg_namelen > 0) {
            return sendto(buffer.data(), buffer.size(), flags,
                          static_cast<const struct sockaddr *>(msg->msg_name),
                          msg->msg_namelen);
        }

        return send(buffer.data(), buffer.size(), flags);
    }

    int socket_file::recvmsg(struct msghdr *msg, int flags)
    {
        if (!msg) {
            return -EFAULT;
        }
        if (!msg->msg_iov || msg->msg_iovlen == 0) {
            return -EINVAL;
        }

        size_t total_len = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            if (!msg->msg_iov[i].iov_base) {
                return -EFAULT;
            }
            total_len += msg->msg_iov[i].iov_len;
        }
        if (total_len == 0) {
            return 0;
        }

        eastl::vector<uint8_t> buffer(total_len);
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        socklen_t src_len = sizeof(src_addr);

        int received = recvfrom(buffer.data(), buffer.size(), flags,
                                msg->msg_name ? reinterpret_cast<struct sockaddr *>(&src_addr) : nullptr,
                                msg->msg_name ? &src_len : nullptr);
        if (received < 0) {
            return received;
        }

        size_t copied = 0;
        for (size_t i = 0; i < msg->msg_iovlen && copied < static_cast<size_t>(received); i++) {
            size_t part = eastl::min(msg->msg_iov[i].iov_len, static_cast<size_t>(received) - copied);
            memcpy(msg->msg_iov[i].iov_base, buffer.data() + copied, part);
            copied += part;
        }

        if (msg->msg_name != nullptr) {
            socklen_t copy_len = eastl::min(msg->msg_namelen, static_cast<socklen_t>(sizeof(src_addr)));
            memcpy(msg->msg_name, &src_addr, copy_len);
            msg->msg_namelen = sizeof(src_addr);
        }
        msg->msg_flags = 0;
        return received;
    }
}

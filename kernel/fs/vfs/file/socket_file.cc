#include "fs/vfs/file/socket_file.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/signal.hh"
#include "tm/timer_manager.hh"
#include <errno.h>
#include "fs/vfs/virtual_fs.hh"
#include "net/f7ly_network.hh"
#include "onps.hh"

namespace fs
{
    namespace
    {
        constexpr int k_loopback_binding_max = 256;
        constexpr uint32 k_loopback_addr = 0x0100007f; // 127.0.0.1 的网络字节序整数表示
        constexpr uint16 k_ephemeral_port_start = 20000;
        constexpr uint16 k_ephemeral_port_end = 60999;
        constexpr int k_protocol_ip = 0;
        constexpr int k_protocol_icmp = 1;
        constexpr int k_protocol_tcp = 6;
        constexpr int k_protocol_udp = 17;
        constexpr int k_protocol_ipv6 = 41;
        constexpr uint8 k_default_ip_ttl = 64;
        constexpr size_t k_ipv4_header_len = 20;
        constexpr int k_ip_recverr = 11;
        constexpr int k_tcp_nodelay = 1;
        constexpr int k_tcp_maxseg = 2;
        constexpr int k_tcp_cork = 3;
        constexpr int k_tcp_keepidle = 4;
        constexpr int k_tcp_keepintvl = 5;
        constexpr int k_tcp_keepcnt = 6;
        constexpr int k_tcp_syncnt = 7;
        constexpr int k_tcp_linger2 = 8;
        constexpr int k_tcp_defer_accept = 9;
        constexpr int k_tcp_window_clamp = 10;
        constexpr int k_tcp_info = 11;
        constexpr int k_tcp_quickack = 12;
        constexpr int k_tcp_congestion = 13;
        constexpr int k_tcp_user_timeout = 18;
        constexpr int k_tcp_default_maxseg = 1460;
        constexpr int k_default_socket_buffer_size = 64 * 1024;
        constexpr size_t k_tcp_recv_buffer_max_bytes = 512 * 1024;
        constexpr size_t k_udp_queue_max_bytes = 256 * 1024;
        constexpr size_t k_udp_queue_max_packets = 256;
        constexpr socklen_t k_max_user_sockaddr_len = 4096;
        constexpr int k_loopback_somaxconn = 4096;
        constexpr uint64 k_tcp_connect_listener_wait_ticks = 100;
        constexpr uint64 k_socket_usec_per_sec = 1000000ULL;
        constexpr int k_unix_binding_max = 256;
        constexpr int k_at_fdcwd = -100;

        struct socket_timeval
        {
            long tv_sec;
            long tv_usec;
        };

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

        int copy_socket_int_option(void *optval, socklen_t *optlen, int value)
        {
            if (*optlen < sizeof(int))
            {
                return -EINVAL;
            }
            *static_cast<int *>(optval) = value;
            *optlen = sizeof(int);
            return 0;
        }

        bool is_receive_timeout_option(int optname)
        {
            return optname == SO_RCVTIMEO ||
                   optname == SO_RCVTIMEO_OLD ||
                   optname == SO_RCVTIMEO_NEW;
        }

        bool is_send_timeout_option(int optname)
        {
            return optname == SO_SNDTIMEO ||
                   optname == SO_SNDTIMEO_OLD ||
                   optname == SO_SNDTIMEO_NEW;
        }

        bool socket_timeout_to_usec(long sec, long usec, uint64 &timeout_us)
        {
            if (sec < 0 || usec < 0 || usec >= static_cast<long>(k_socket_usec_per_sec))
            {
                return false;
            }
            if (static_cast<uint64>(sec) > UINT64_MAX / k_socket_usec_per_sec)
            {
                timeout_us = UINT64_MAX;
                return true;
            }
            uint64 base = static_cast<uint64>(sec) * k_socket_usec_per_sec;
            timeout_us = base > UINT64_MAX - static_cast<uint64>(usec)
                             ? UINT64_MAX
                             : base + static_cast<uint64>(usec);
            return true;
        }

        uint64 socket_now_usec()
        {
            tmm::timeval tv = tmm::k_tm.get_time_val();
            return tv.tv_sec * k_socket_usec_per_sec + tv.tv_usec;
        }

        int copy_socket_timeval_option(void *optval, socklen_t *optlen, long sec, long usec)
        {
            if (*optlen < sizeof(socket_timeval))
            {
                return -EINVAL;
            }
            socket_timeval value{};
            value.tv_sec = sec;
            value.tv_usec = usec;
            memcpy(optval, &value, sizeof(value));
            *optlen = sizeof(value);
            return 0;
        }

        int normalize_socket_type(int type)
        {
            return type & 0b111;
        }

        bool is_loopback_or_any(uint32 addr)
        {
            return addr == 0 || addr == k_loopback_addr || addr == 0x7f000001;
        }

        bool is_ipv6_any(const struct in6_addr &addr)
        {
            for (int i = 0; i < 16; ++i)
            {
                if (addr.s6_addr[i] != 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool is_ipv6_loopback(const struct in6_addr &addr)
        {
            for (int i = 0; i < 15; ++i)
            {
                if (addr.s6_addr[i] != 0)
                {
                    return false;
                }
            }
            return addr.s6_addr[15] == 1;
        }

        bool is_ipv4_mapped_ipv6(const struct in6_addr &addr)
        {
            for (int i = 0; i < 10; ++i)
            {
                if (addr.s6_addr[i] != 0)
                {
                    return false;
                }
            }
            return addr.s6_addr[10] == 0xff && addr.s6_addr[11] == 0xff;
        }

        bool sockaddr_in6_to_loopback_in(const struct sockaddr_in6 &addr6, struct sockaddr_in &addr4)
        {
            memset(&addr4, 0, sizeof(addr4));
            addr4.sin_family = AF_INET;
            addr4.sin_port = addr6.sin6_port;

            if (is_ipv6_any(addr6.sin6_addr))
            {
                addr4.sin_addr = 0;
                return true;
            }
            if (is_ipv6_loopback(addr6.sin6_addr))
            {
                addr4.sin_addr = k_loopback_addr;
                return true;
            }
            if (is_ipv4_mapped_ipv6(addr6.sin6_addr))
            {
                memcpy(&addr4.sin_addr, &addr6.sin6_addr.s6_addr[12], sizeof(addr4.sin_addr));
                return is_loopback_or_any(addr4.sin_addr);
            }
            return false;
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
            bool has_same_port = false;
            for (auto &binding : g_loopback_bindings)
            {
                if (!binding.used || binding.type != type || binding.port != port)
                {
                    continue;
                }
                has_same_port = true;
                // iperf3 UDP server 会把旧 listener connect() 到客户端后，
                // 立即在同一端口创建新的 UDP listener。Linux 允许“已连接
                // UDP socket + 未连接 listener”共用本地端口；但普通重复
                // bind 仍必须返回 EADDRINUSE。
                if (type != SocketType::UDP || binding.socket == nullptr ||
                    binding.socket->get_state() != SocketState::CONNECTED)
                {
                    return -EADDRINUSE;
                }
            }
            if (has_same_port && type != SocketType::UDP)
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

        bool can_use_onps_socket(SocketFamily family, SocketType type)
        {
            return family == SocketFamily::INET &&
                   (type == SocketType::TCP || type == SocketType::UDP) &&
                   net::is_network_stack_ready();
        }

        bool can_use_onps_raw_icmp(SocketFamily family, SocketType type, int protocol)
        {
            return family == SocketFamily::INET &&
                   type == SocketType::RAW &&
                   protocol == k_protocol_icmp &&
                   net::is_network_stack_ready();
        }

        bool should_route_via_onps(SocketFamily family, SocketType type, uint32 addr)
        {
            return can_use_onps_socket(family, type) && !is_loopback_or_any(addr);
        }

        int onps_error_to_errno(EN_ONPSERR error)
        {
            switch (error)
            {
                case ERRNO:
                    return 0;
                case ERRNOFREEMEM:
                case ERRNOPAGENODE:
                case ERRNOBUFLISTNODE:
                case ERRNEWARPCTLBLOCK:
                case ERRNONETIFNODE:
                case ERRNOROUTENODE:
                case ERRNOSOCKET:
                case ERRNOTCPLINKNODE:
                case ERRNOUDPLINKNODE:
                case ERRTCPSRVEMPTY:
                case ERRTCPBACKLOGEMPTY:
                case ERRTCPRCVQUEUEEMPTY:
                    return -ENOBUFS;
                case ERRPORTOCCUPIED:
                    return -EADDRINUSE;
                case ERRADDRESSING:
                case ERRNETUNREACHABLE:
                case ERRROUTEADDRMATCH:
                case ERRNETIFNOTFOUND:
                case ERRNONETIFFOUND:
                    return -ENETUNREACH;
                case ERRADDRFAMILIES:
                case ERRUNSUPPORTEDFAMILY:
                case ERRFAMILYINCONSISTENT:
                    return -EAFNOSUPPORT;
                case ERRSOCKETTYPE:
                case ERRUNSUPPIPPROTO:
                case ERRIPROTOMATCH:
                case ERRTCPONLY:
                    return -EOPNOTSUPP;
                case ERRTCPCONNTIMEOUT:
                case ERRTCPACKTIMEOUT:
                case ERRWAITACKTIMEOUT:
                    return -ETIMEDOUT;
                case ERRTCPCONNRESET:
                    return -ECONNRESET;
                case ERRTCPCONNCLOSED:
                    return -EPIPE;
                case ERRTCPNOTCONNECTED:
                case ERRNOTBINDADDR:
                    return -ENOTCONN;
                case ERRPACKETTOOLARGE:
                    return -EMSGSIZE;
                case ERRTCPBACKLOGFULL:
                    return -EAGAIN;
                case ERRDATAEMPTY:
                case ERRSENDZEROBYTES:
                case ERRPORTEMPTY:
                    return -EINVAL;
                default:
                    return -EIO;
            }
        }

        int onps_last_errno(SOCKET socket)
        {
            EN_ONPSERR error = socket_get_last_error_code(socket);
            int result = onps_error_to_errno(error);
            return result == 0 ? -EIO : result;
        }

        int onps_socket_type(SocketType type)
        {
            return type == SocketType::TCP ? SOCK_STREAM : SOCK_DGRAM;
        }

        void close_onps_handle(SocketType type, SOCKET socket)
        {
            if (socket == INVALID_SOCKET)
            {
                return;
            }
            if (type == SocketType::RAW)
            {
                onps_input_free(static_cast<INT>(socket));
                return;
            }
            ::close(socket);
        }

        uint16 socket_port_to_host(uint16 net_port)
        {
            return to_network_u16(net_port);
        }

        uint16 socket_port_to_network(uint16 host_port)
        {
            return to_network_u16(host_port);
        }

        uint16 internet_checksum(const uint8_t *data, size_t len)
        {
            uint32 sum = 0;
            for (size_t i = 0; i + 1 < len; i += 2)
            {
                sum += static_cast<uint16>((static_cast<uint16>(data[i]) << 8) | data[i + 1]);
                sum = (sum & 0xffff) + (sum >> 16);
            }
            if ((len & 1) != 0)
            {
                sum += static_cast<uint16>(data[len - 1] << 8);
                sum = (sum & 0xffff) + (sum >> 16);
            }
            while ((sum >> 16) != 0)
            {
                sum = (sum & 0xffff) + (sum >> 16);
            }
            return static_cast<uint16>(~sum);
        }

        struct raw_ipv4_header
        {
            uint8_t version_ihl;
            uint8_t tos;
            uint16_t total_len;
            uint16_t id;
            uint16_t frag_off;
            uint8_t ttl;
            uint8_t protocol;
            uint16_t checksum;
            uint32_t src;
            uint32_t dst;
        } __attribute__((packed));

        void build_raw_ipv4_header(raw_ipv4_header &header, uint32_t src, uint32_t dst,
                                   uint8_t ttl, uint16_t payload_len)
        {
            memset(&header, 0, sizeof(header));
            header.version_ihl = 0x45;
            header.total_len = socket_port_to_network(static_cast<uint16>(k_ipv4_header_len + payload_len));
            header.ttl = ttl;
            header.protocol = k_protocol_icmp;
            header.src = src;
            header.dst = dst;
            header.checksum = socket_port_to_network(internet_checksum(reinterpret_cast<const uint8_t *>(&header),
                                                                       sizeof(header)));
        }

        void ipv4_to_string(uint32 addr, char out[16])
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&addr);
            snprintf(out, 16, "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
        }

        void refresh_onps_local_addr(SOCKET socket, struct sockaddr_in &local_addr)
        {
            EN_ONPSERR error = ERRNO;
            PST_TCPUDP_HANDLE handle = nullptr;
            if (!onps_input_get(static_cast<INT>(socket), IOPT_GETTCPUDPADDR, &handle, &error) ||
                handle == nullptr)
            {
                return;
            }

            local_addr.sin_family = AF_INET;
            local_addr.sin_port = socket_port_to_network(handle->stSockAddr.usPort);
            local_addr.sin_addr = handle->stSockAddr.saddr_ipv4;
            if (local_addr.sin_addr == 0)
            {
                local_addr.sin_addr = inet_addr("10.0.2.15");
            }
        }

        CHAR onps_recv_timeout_seconds(bool nonblocking, long sec, long usec)
        {
            if (nonblocking)
            {
                return 0;
            }
            if (sec == 0 && usec == 0)
            {
                return -1;
            }
            long timeout = sec + (usec > 0 ? 1 : 0);
            if (timeout <= 0)
            {
                return 0;
            }
            if (timeout > 126)
            {
                timeout = 126;
            }
            return static_cast<CHAR>(timeout);
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
        , _datagram_queue_bytes(0)
        , _blocking(true)
        , _reuse_addr(false)
        , _loopback_registered(false)
        , _unix_registered(false)
        , _onps_active(false)
        , _onps_bound(false)
        , _onps_listening(false)
        , _read_shutdown(false)
        , _write_shutdown(false)
        , _peer_closed(false)
        , _pending_send_has_addr(false)
        , _recv_timeout_sec(0)
        , _recv_timeout_usec(0)
        , _send_timeout_sec(0)
        , _send_timeout_usec(0)
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
        , _datagram_queue_bytes(0)
        , _blocking(true)
        , _reuse_addr(false)
        , _loopback_registered(false)
        , _unix_registered(false)
        , _onps_active(false)
        , _onps_bound(false)
        , _onps_listening(false)
        , _read_shutdown(false)
        , _write_shutdown(false)
        , _peer_closed(false)
        , _pending_send_has_addr(false)
        , _recv_timeout_sec(0)
        , _recv_timeout_usec(0)
        , _send_timeout_sec(0)
        , _send_timeout_usec(0)
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
        SOCKET onps_socket = _onps_socket;
        _onps_socket = INVALID_SOCKET;
        _onps_active = false;
        _onps_bound = false;
        _onps_listening = false;
        proc::k_pm.wakeup(&_pending_connections);
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
        _lock.release();

        if (onps_socket != INVALID_SOCKET)
        {
            close_onps_handle(_type, onps_socket);
        }

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
                if (_onps_active)
                {
                    result = !_read_shutdown;
                    break;
                }
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
                result = !_pending_connections.empty() || _onps_listening;
                break;
            case SocketState::BOUND:
                result = _type == SocketType::UDP &&
                         (!_datagram_queue.empty() || (_onps_bound && !_loopback_registered));
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
            if (_onps_active)
            {
                result = !_write_shutdown;
                _lock.release();
                return result;
            }
            if (_type == SocketType::TCP)
            {
                socket_file *peer = _peer;
                bool local_ready = !_write_shutdown && !_peer_closed && peer != nullptr;
                _lock.release();
                if (!local_ready)
                {
                    return false;
                }

                peer->_lock.acquire();
                // TCP 写就绪必须反映对端接收队列空间；否则 poll/select 会在队列已满时
                // 继续驱动写入，iperf 这类吞吐工具会把内核堆推到无限扩容。
                result = peer->_read_shutdown || peer->_state == SocketState::CLOSED ||
                         peer->_recv_buffer.size() < k_tcp_recv_buffer_max_bytes;
                peer->_lock.release();
                return result;
            }
            result = !_write_shutdown && !_peer_closed && (_type == SocketType::UDP || _peer != nullptr);
        }
        else if (_type == SocketType::UDP && (_state == SocketState::CREATED || _state == SocketState::BOUND))
        {
            result = !_write_shutdown;
        }
        _lock.release();
        return result;
    }

    bool socket_file::epoll_rdhup_ready() const
    {
        auto *self = const_cast<socket_file *>(this);
        self->_lock.acquire();
        // EPOLLRDHUP 只对面向连接的字节流语义有意义：
        // 1. 本端 shutdown(SHUT_RD) 后，读半边已经挂起；
        // 2. 对端关闭写半边/连接后，本端会看到 peer closed。
        // LTP epoll_wait05 就依赖这两类状态都能被 epoll 观察到。
        bool ready = _state == SocketState::CONNECTED &&
                     _type == SocketType::TCP &&
                     (_read_shutdown || _peer_closed);
        self->_lock.release();
        return ready;
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

        if (_family != SocketFamily::INET && _family != SocketFamily::INET6) {
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
        if (_family == SocketFamily::INET6) {
            struct sockaddr_in6 local_addr6;
            memcpy(&local_addr6, addr, sizeof(local_addr6));
            if (local_addr6.sin6_family != AF_INET6 ||
                !sockaddr_in6_to_loopback_in(local_addr6, local_addr)) {
                _lock.release();
                return -EAFNOSUPPORT;
            }
        } else {
            memcpy(&local_addr, addr, sizeof(local_addr));
            if (local_addr.sin_family != AF_INET) {
                _lock.release();
                return -EAFNOSUPPORT;
            }
        }
        bool bind_loopback = is_loopback_or_any(local_addr.sin_addr);
        bool bind_onps = can_use_onps_socket(_family, _type) &&
                         (local_addr.sin_addr == 0 || !bind_loopback);
        if (!bind_loopback && !bind_onps) {
            _lock.release();
            return -EADDRNOTAVAIL;
        }

        if (local_addr.sin_port == 0) {
            ensure_loopback_table();
            g_loopback_lock.acquire();
            local_addr.sin_port = allocate_ephemeral_port(_type);
            if (local_addr.sin_port == 0) {
                g_loopback_lock.release();
                _lock.release();
                return -EADDRINUSE;
            }
            g_loopback_lock.release();
        }

        if (bind_loopback) {
            ensure_loopback_table();
            g_loopback_lock.acquire();
            int result = register_loopback_binding(_type, local_addr.sin_port, this);
            if (result < 0) {
                g_loopback_lock.release();
                _lock.release();
                return result;
            }
            _loopback_registered = true;
            g_loopback_lock.release();
        }

        _local_addr = local_addr;
        if (bind_onps) {
            int onps_result = bind_onps_locked(local_addr);
            if (onps_result < 0) {
                if (_loopback_registered) {
                    g_loopback_lock.acquire();
                    unregister_loopback_binding(_type, _local_addr.sin_port, this);
                    g_loopback_lock.release();
                    _loopback_registered = false;
                }
                _lock.release();
                return onps_result;
            }
        }
        _state = SocketState::BOUND;
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

        // Linux 会把超大 backlog 静默截到 somaxconn。iperf 等程序常传入
        // INT_MAX，如果直接 reserve(backlog) 会把用户参数放大成巨额内核堆申请。
        _backlog = backlog > 0 ? backlog : 1;
        if (_backlog > k_loopback_somaxconn) {
            _backlog = k_loopback_somaxconn;
        }
        if (_onps_bound && !_onps_listening) {
            int onps_backlog = _backlog > TCPSRV_BACKLOG_NUM_MAX ? TCPSRV_BACKLOG_NUM_MAX : _backlog;
            if (::listen(_onps_socket, static_cast<USHORT>(onps_backlog)) != 0) {
                int result = onps_last_errno(_onps_socket);
                _lock.release();
                return result;
            }
            _onps_listening = true;
        }
        _state = SocketState::LISTENING;
        _pending_connections.reserve(_backlog);
        
        _lock.release();
        return 0;
    }

    int socket_file::accept(struct sockaddr *addr, socklen_t *addrlen, socket_file **accepted_socket)
    {
        if (accepted_socket == nullptr) {
            return -EFAULT;
        }
        *accepted_socket = nullptr;

        _lock.acquire();
        
        if (_state != SocketState::LISTENING) {
            _lock.release();
            return -EINVAL;
        }

        proc::Pcb *cur = proc::k_pm.get_cur_pcb();
        // 检查是否有待处理的连接
        while (_pending_connections.empty()) {
            if (_onps_listening) {
                SOCKET listen_socket = _onps_socket;
                bool blocking = _blocking;
                _lock.release();

                UINT client_ip = 0;
                USHORT client_port = 0;
                EN_ONPSERR error = ERRNO;
                SOCKET client = ::accept(listen_socket, &client_ip, &client_port,
                                         blocking ? 1 : 0, &error);
                if (client != INVALID_SOCKET) {
                    socket_file *server_side = new socket_file(AF_INET, SOCK_STREAM, _protocol);
                    if (server_side == nullptr) {
                        ::close(client);
                        return -ENOMEM;
                    }

                    server_side->_onps_socket = client;
                    server_side->_onps_active = true;
                    server_side->_onps_bound = true;
                    server_side->_state = SocketState::CONNECTED;
                    server_side->_local_addr = _local_addr;
                    refresh_onps_local_addr(client, server_side->_local_addr);
                    memset(&server_side->_remote_addr, 0, sizeof(server_side->_remote_addr));
                    server_side->_remote_addr.sin_family = AF_INET;
                    server_side->_remote_addr.sin_addr = client_ip;
                    server_side->_remote_addr.sin_port = socket_port_to_network(client_port);

                    if (addr && addrlen && *addrlen > 0) {
                        socklen_t copy_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                        memcpy(addr, &server_side->_remote_addr, copy_len);
                        *addrlen = sizeof(struct sockaddr_in);
                    }
                    *accepted_socket = server_side;
                    return 0;
                }

                if (!blocking) {
                    return -EAGAIN;
                }
                if (error != ERRNO) {
                    int mapped = onps_error_to_errno(error);
                    if (mapped != -EIO) {
                        return mapped;
                    }
                }

                _lock.acquire();
                if (_state != SocketState::LISTENING) {
                    _lock.release();
                    return -EINVAL;
                }
                continue;
            }
            // accept(2) 是信号可中断的阻塞系统调用。netperf TCP_CRR 的
            // server 依赖 ITIMER_REAL/SIGALRM 打断这里的空队列等待后汇报结果。
            if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                _lock.release();
                return -EINTR;
            }
            if (!_blocking) {
                _lock.release();
                return -EAGAIN;
            }
            proc::k_pm.sleep(&_pending_connections, &_lock);
            if (_state != SocketState::LISTENING) {
                _lock.release();
                return -EINVAL;
            }
        }

        // 获取一个待处理的连接
        socket_file* client_socket = get_from_pending_queue();
        if (!client_socket) {
            _lock.release();
            return -EAGAIN;
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
        *accepted_socket = client_socket;
        _lock.release();
        return 0;
    }

    int socket_file::connect(const struct sockaddr *addr, socklen_t addrlen)
    {
        if (!addr || addrlen < sizeof(struct sockaddr)) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (_state != SocketState::CREATED && _state != SocketState::BOUND) {
            _lock.release();
            return -EISCONN;
        }
        // 根据 socket 族类型处理不同的连接方式
        if (_family == SocketFamily::UNIX) {
            // Unix domain socket 连接
            constexpr socklen_t k_unix_addr_prefix_len =
                static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path));
            // Linux 允许只传“family + 实际路径长度”这一段 sockaddr_un。
            // 这里不能强行要求满 108 字节 sun_path，否则 musl 的 nscd 探测会直接得到 EINVAL。
            if (addrlen < k_unix_addr_prefix_len + 1) {
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

        } else if (_family == SocketFamily::INET || _family == SocketFamily::INET6) {
            struct sockaddr generic_addr;
            memcpy(&generic_addr, addr, sizeof(generic_addr));
            struct sockaddr_in remote_addr;
            if (_family == SocketFamily::INET6 && generic_addr.sa_family == AF_INET) {
                // 双栈 IPv6 socket 在 UDP accept 路径中可能会对 IPv4 peer
                // 调用 connect()。loopback 后端统一落到 IPv4 端口表，因此这里
                // 接受 AF_INET peer，避免把合法的 127.0.0.1 源地址误判为 EINVAL。
                if (addrlen < sizeof(struct sockaddr_in)) {
                    _lock.release();
                    return -EINVAL;
                }
                memcpy(&remote_addr, addr, sizeof(remote_addr));
            } else if (_family == SocketFamily::INET6) {
                if (addrlen < sizeof(struct sockaddr_in6)) {
                    _lock.release();
                    return -EINVAL;
                }
                struct sockaddr_in6 remote_addr6;
                memcpy(&remote_addr6, addr, sizeof(remote_addr6));
                if (remote_addr6.sin6_family != AF_INET6 ||
                    !sockaddr_in6_to_loopback_in(remote_addr6, remote_addr)) {
                    _lock.release();
                    return -EAFNOSUPPORT;
                }
            } else {
                if (addrlen < sizeof(struct sockaddr_in)) {
                    _lock.release();
                    return -EINVAL;
                }
                memcpy(&remote_addr, addr, sizeof(remote_addr));
                if (remote_addr.sin_family != AF_INET) {
                    _lock.release();
                    return -EAFNOSUPPORT;
                }
            }
            if (!is_loopback_or_any(remote_addr.sin_addr)) {
                if (!should_route_via_onps(_family, _type, remote_addr.sin_addr)) {
                    _lock.release();
                    return -ENETUNREACH;
                }

                int ensure_result = ensure_onps_socket_locked();
                if (ensure_result < 0) {
                    _lock.release();
                    return ensure_result;
                }
                if (_state == SocketState::BOUND && !_onps_bound) {
                    int bind_result = bind_onps_locked(_local_addr);
                    if (bind_result < 0) {
                        _lock.release();
                        return bind_result;
                    }
                }

                SOCKET onps_socket = _onps_socket;
                USHORT host_port = socket_port_to_host(remote_addr.sin_port);
                bool nonblocking = is_nonblocking_request(0);
                _lock.release();

                int result;
                if (_type == SocketType::TCP && nonblocking) {
                    result = ::connect_nb_ext(onps_socket, &remote_addr.sin_addr, host_port);
                    if (result == 1) {
                        return -EINPROGRESS;
                    }
                } else {
                    result = ::connect_ext(onps_socket, &remote_addr.sin_addr, host_port,
                                           _type == SocketType::TCP ? TCP_CONN_TIMEOUT : 0);
                }

                if (result != 0) {
                    return onps_last_errno(onps_socket);
                }

                _lock.acquire();
                _remote_addr = remote_addr;
                _onps_active = true;
                _state = SocketState::CONNECTED;
                refresh_onps_local_addr(_onps_socket, _local_addr);
                _lock.release();
                return 0;
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
            socket_file *listener = nullptr;
            const uint64 listener_wait_start_tick = tmm::k_tm.get_ticks();
            for (;;) {
                g_loopback_lock.acquire();
                loopback_binding *binding = find_loopback_binding(SocketType::TCP, remote_addr.sin_port);
                listener = binding ? binding->socket : nullptr;
                if (listener != nullptr) {
                    listener->_lock.acquire();
                    if (listener->_state == SocketState::LISTENING && listener->can_accept_connection()) {
                        break;
                    }
                    listener->_lock.release();
                    listener = nullptr;
                }
                g_loopback_lock.release();

                uint64 waited_ticks = tmm::k_tm.get_ticks() - listener_wait_start_tick;
                if (!_blocking || waited_ticks >= k_tcp_connect_listener_wait_ticks) {
                    _lock.release();
                    return -ECONNREFUSED;
                }

                proc::Pcb *cur = proc::k_pm.get_cur_pcb();
                if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                    _lock.release();
                    return -EINTR;
                }

                // netperf/iperf 这类脚本会用“后台 server & 前台 client”的模式。
                // 只 yield 时，当前进程可能马上再次被选中，后台 netserver 还没
                // 完成 exec/bind/listen；这里睡到下一个 tick，让启动方真正获得
                // 运行窗口。等待仍有上限，无监听端口最终保持 ECONNREFUSED。
                proc::k_pm.sleep(tmm::k_tm.get_tick_wait_channel(), &_lock);
                if (_state != SocketState::CREATED && _state != SocketState::BOUND) {
                    _lock.release();
                    return _state == SocketState::CONNECTED ? -EISCONN : -ECONNABORTED;
                }
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
        if (len == 0) {
            return 0;
        }
        if (!buf) {
            return -EFAULT;
        }

        _lock.acquire();

        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            if (_onps_active) {
                if (_write_shutdown) {
                    _lock.release();
                    return -EPIPE;
                }
                SOCKET onps_socket = _onps_socket;
                bool nonblocking = is_nonblocking_request(flags);
                _lock.release();

                INT request_len = len > static_cast<size_t>(INT_MAX)
                                      ? INT_MAX
                                      : static_cast<INT>(len);
                for (;;) {
                    INT sent = ::send_nb(onps_socket, const_cast<UCHAR *>(data), request_len);
                    if (sent > 0) {
                        return sent;
                    }
                    if (sent < 0) {
                        return onps_last_errno(onps_socket);
                    }
                    if (nonblocking) {
                        return -EAGAIN;
                    }
                    proc::Pcb *cur = proc::k_pm.get_cur_pcb();
                    if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                        return -EINTR;
                    }
                    os_sleep_ms(1);
                }
            }
            if (_write_shutdown || _peer == nullptr || _peer_closed) {
                _lock.release();
                return -EPIPE;
            }
            socket_file *peer = _peer;
            eastl::vector<uint8_t> flush_buffer;
            const uint8_t *send_data = data;
            size_t send_len = len;
            size_t pending_len = 0;
            bool had_pending = false;
            bool nonblocking = is_nonblocking_request(flags);

            if (flags & MSG_MORE) {
                int append_result = append_pending_send_locked(data, len, nullptr);
                _lock.release();
                return append_result;
            }

            if (!_send_buffer.empty()) {
                pending_len = _send_buffer.size();
                had_pending = true;
                flush_buffer.reserve(_send_buffer.size() + len);
                flush_buffer.insert(flush_buffer.end(), _send_buffer.begin(), _send_buffer.end());
                flush_buffer.insert(flush_buffer.end(), data, data + len);
                send_data = flush_buffer.data();
                send_len = flush_buffer.size();
            }
            _lock.release();

            if (had_pending && nonblocking)
            {
                peer->_lock.acquire();
                bool peer_broken = peer->_read_shutdown || peer->_state == SocketState::CLOSED;
                size_t used = peer->_recv_buffer.size();
                bool can_flush_now = !peer_broken && used <= k_tcp_recv_buffer_max_bytes &&
                                     send_len <= k_tcp_recv_buffer_max_bytes - used;
                peer->_lock.release();
                if (peer_broken)
                {
                    return -EPIPE;
                }
                if (!can_flush_now)
                {
                    return -EAGAIN;
                }
            }

            if (had_pending)
            {
                _lock.acquire();
                _send_buffer.clear();
                _pending_send_has_addr = false;
                _lock.release();
            }

            // 只在本端锁内读取连接状态；实际入队时只持有对端锁，避免双向 send 互相等待。
            int queued = enqueue_stream_data_to_peer(peer, send_data, send_len, nonblocking);
            if (queued < 0)
            {
                return queued;
            }
            if (!had_pending)
            {
                return queued;
            }
            if (static_cast<size_t>(queued) >= send_len)
            {
                return static_cast<int>(len);
            }
            if (static_cast<size_t>(queued) > pending_len)
            {
                return static_cast<int>(static_cast<size_t>(queued) - pending_len);
            }
            return -EPIPE;
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
        if (len == 0) {
            return 0;
        }
        if (!buf) {
            return -EFAULT;
        }

        _lock.acquire();

        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            if (_onps_active) {
                if (_read_shutdown) {
                    _lock.release();
                    return 0;
                }
                int timeout_result = configure_onps_recv_timeout_locked(flags);
                if (timeout_result < 0) {
                    _lock.release();
                    return timeout_result;
                }
                SOCKET onps_socket = _onps_socket;
                bool nonblocking = is_nonblocking_request(flags);
                bool has_timeout = _recv_timeout_sec != 0 || _recv_timeout_usec != 0;
                _lock.release();

                INT request_len = len > static_cast<size_t>(INT_MAX)
                                      ? INT_MAX
                                      : static_cast<INT>(len);
                INT received = ::recv(onps_socket, data, request_len);
                if (received > 0) {
                    return received;
                }
                if (received == 0) {
                    return (nonblocking || has_timeout) ? -EAGAIN : 0;
                }
                return onps_last_errno(onps_socket);
            }

            proc::Pcb *cur = proc::k_pm.get_cur_pcb();
            uint64 timeout_us = 0;
            bool has_timeout = socket_timeout_to_usec(_recv_timeout_sec, _recv_timeout_usec, timeout_us) &&
                               timeout_us > 0;
            uint64 deadline_us = has_timeout ? socket_now_usec() + timeout_us : 0;
            while (_recv_buffer.empty() && !_peer_closed && !_read_shutdown) {
                if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                    _lock.release();
                    return -EINTR;
                }
                if (is_nonblocking_request(flags)) {
                    _lock.release();
                    return -EAGAIN;
                }
                if (has_timeout) {
                    if (socket_now_usec() >= deadline_us) {
                        _lock.release();
                        return -EAGAIN;
                    }
                    // SO_RCVTIMEO 需要超时唤醒；睡 tick 通道可避免无数据时永久挂住。
                    proc::k_pm.sleep(tmm::k_tm.get_tick_wait_channel(), &_lock);
                } else {
                    proc::k_pm.sleep(&_recv_buffer, &_lock);
                }
            }

            if (_recv_buffer.empty()) {
                _lock.release();
                return 0;
            }

            size_t copy_len = eastl::min(len, _recv_buffer.size());
            memcpy(data, _recv_buffer.data(), copy_len);
            if (!(flags & MSG_PEEK)) {
                _recv_buffer.erase(_recv_buffer.begin(), _recv_buffer.begin() + copy_len);
                // 读端释放接收队列空间后唤醒阻塞写端，形成 TCP loopback 背压闭环。
                proc::k_pm.wakeup(&_recv_buffer);
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
        if (len > 0 && !buf) {
            return -EFAULT;
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

            bool route_onps = !is_loopback_or_any(dest.sin_addr);
            if (route_onps) {
                if (!should_route_via_onps(_family, _type, dest.sin_addr)) {
                    _lock.release();
                    return -ENETUNREACH;
                }
                int ensure_result = ensure_onps_socket_locked();
                if (ensure_result < 0) {
                    _lock.release();
                    return ensure_result;
                }
                if (_state == SocketState::BOUND && !_onps_bound) {
                    int bind_result = bind_onps_locked(_local_addr);
                    if (bind_result < 0) {
                        _lock.release();
                        return bind_result;
                    }
                }
                int source_result = ensure_onps_udp_source_locked(dest.sin_addr);
                if (source_result < 0) {
                    _lock.release();
                    return source_result;
                }
            }

            if (!route_onps && _state == SocketState::CREATED) {
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

            if (route_onps) {
                SOCKET onps_socket = _onps_socket;
                char dest_ip[16];
                ipv4_to_string(send_dest.sin_addr, dest_ip);
                USHORT host_port = socket_port_to_host(send_dest.sin_port);
                _lock.release();

                INT sent = ::sendto(onps_socket, dest_ip, host_port,
                                    const_cast<UCHAR *>(send_data),
                                    send_len > static_cast<size_t>(INT_MAX)
                                        ? INT_MAX
                                        : static_cast<INT>(send_len));
                if (sent < 0) {
                    return onps_last_errno(onps_socket);
                }

                _lock.acquire();
                _onps_active = true;
                if (_state == SocketState::CREATED) {
                    _state = SocketState::BOUND;
                }
                refresh_onps_local_addr(_onps_socket, _local_addr);
                _lock.release();
                return sent;
            }

            _lock.release();

            socket_file *target = nullptr;
            g_loopback_lock.acquire();
            socket_file *fallback_listener = nullptr;
            struct sockaddr_in normalized_src = src;
            if (normalized_src.sin_addr == 0) {
                normalized_src.sin_addr = k_loopback_addr;
            }
            for (auto &binding : g_loopback_bindings) {
                if (!binding.used || binding.type != SocketType::UDP ||
                    binding.port != send_dest.sin_port || binding.socket == nullptr) {
                    continue;
                }

                socket_file *candidate = binding.socket;
                if (candidate->_state == SocketState::CONNECTED) {
                    struct sockaddr_in remote = candidate->_remote_addr;
                    if (remote.sin_addr == 0) {
                        remote.sin_addr = k_loopback_addr;
                    }
                    if (remote.sin_port == normalized_src.sin_port &&
                        is_loopback_or_any(remote.sin_addr) &&
                        is_loopback_or_any(normalized_src.sin_addr)) {
                        target = candidate;
                        break;
                    }
                    continue;
                }

                if (fallback_listener == nullptr) {
                    fallback_listener = candidate;
                }
            }
            if (target == nullptr) {
                target = fallback_listener;
            }
            g_loopback_lock.release();

            if (target != nullptr) {
                target->_lock.acquire();
                target->enqueue_datagram(&src, send_data, send_len);
                target->_lock.release();
            }
            return static_cast<int>(len);
        } else if (_type == SocketType::RAW) {
            if (_family != SocketFamily::INET || _protocol != k_protocol_icmp) {
                _lock.release();
                return -EOPNOTSUPP;
            }
            if (len < sizeof(ST_ICMP_HDR) + sizeof(ST_ICMP_ECHO_HDR)) {
                _lock.release();
                return -EINVAL;
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

            if (!can_use_onps_raw_icmp(_family, _type, _protocol) ||
                is_loopback_or_any(dest.sin_addr)) {
                _lock.release();
                return -ENETUNREACH;
            }

            int ensure_result = ensure_onps_raw_icmp_locked();
            if (ensure_result < 0) {
                _lock.release();
                return ensure_result;
            }

            const auto *icmp_hdr = reinterpret_cast<const ST_ICMP_HDR *>(data);
            if (icmp_hdr->ubType != ICMP_ECHOREQ || icmp_hdr->ubCode != 0) {
                _lock.release();
                return -EOPNOTSUPP;
            }
            const auto *echo_hdr = reinterpret_cast<const ST_ICMP_ECHO_HDR *>(data + sizeof(ST_ICMP_HDR));
            const UCHAR *payload = data + sizeof(ST_ICMP_HDR) + sizeof(ST_ICMP_ECHO_HDR);
            size_t payload_len = len - sizeof(ST_ICMP_HDR) - sizeof(ST_ICMP_ECHO_HDR);
            if (payload_len > UINT_MAX) {
                _lock.release();
                return -EMSGSIZE;
            }

            SOCKET onps_socket = _onps_socket;
            USHORT identifier = socket_port_to_host(echo_hdr->usIdentifier);
            USHORT sequence = socket_port_to_host(echo_hdr->usSeqNum);
            UINT source_ip = route_get_netif_ip(dest.sin_addr);
            if (source_ip == 0) {
                _lock.release();
                return -ENETUNREACH;
            }
            _local_addr.sin_family = AF_INET;
            _local_addr.sin_addr = source_ip;
            _remote_addr = dest;
            _lock.release();

            EN_ONPSERR error = ERRNO;
            INT sent = icmp_send_echo_reqest(static_cast<INT>(onps_socket),
                                             identifier,
                                             sequence,
                                             k_default_ip_ttl,
                                             dest.sin_addr,
                                             payload,
                                             static_cast<UINT>(payload_len),
                                             &error);
            if (sent < 0) {
                return onps_error_to_errno(error);
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
        if (len == 0) {
            return 0;
        }
        if (!buf) {
            return -EFAULT;
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

            if (_onps_active || (_onps_bound && !_loopback_registered)) {
                int timeout_result = configure_onps_recv_timeout_locked(flags);
                if (timeout_result < 0) {
                    _lock.release();
                    return timeout_result;
                }
                SOCKET onps_socket = _onps_socket;
                bool nonblocking = is_nonblocking_request(flags);
                bool has_timeout = _recv_timeout_sec != 0 || _recv_timeout_usec != 0;
                _lock.release();

                UINT from_ip = 0;
                USHORT from_port = 0;
                INT request_len = len > static_cast<size_t>(INT_MAX)
                                      ? INT_MAX
                                      : static_cast<INT>(len);
                INT received = ::recvfrom(onps_socket, data, request_len, &from_ip, &from_port);
                if (received > 0) {
                    if (src_addr && addrlen && *addrlen > 0) {
                        struct sockaddr_in from{};
                        from.sin_family = AF_INET;
                        from.sin_addr = from_ip;
                        from.sin_port = socket_port_to_network(from_port);
                        socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(from)));
                        memcpy(src_addr, &from, copy_addr_len);
                        *addrlen = sizeof(from);
                    }
                    return received;
                }
                if (received == 0) {
                    return (nonblocking || has_timeout) ? -EAGAIN : 0;
                }
                return onps_last_errno(onps_socket);
            }

            proc::Pcb *cur = proc::k_pm.get_cur_pcb();
            uint64 timeout_us = 0;
            bool has_timeout = socket_timeout_to_usec(_recv_timeout_sec, _recv_timeout_usec, timeout_us) &&
                               timeout_us > 0;
            uint64 deadline_us = has_timeout ? socket_now_usec() + timeout_us : 0;
            while (_datagram_queue.empty() && !_read_shutdown) {
                if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                    _lock.release();
                    return -EINTR;
                }
                if (is_nonblocking_request(flags)) {
                    _lock.release();
                    return -EAGAIN;
                }
                if (has_timeout) {
                    if (socket_now_usec() >= deadline_us) {
                        _lock.release();
                        return -EAGAIN;
                    }
                    proc::k_pm.sleep(tmm::k_tm.get_tick_wait_channel(), &_lock);
                } else {
                    proc::k_pm.sleep(&_datagram_queue, &_lock);
                }
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
                if (_datagram_queue_bytes >= packet.data.size()) {
                    _datagram_queue_bytes -= packet.data.size();
                } else {
                    _datagram_queue_bytes = 0;
                }
                _datagram_queue.erase(_datagram_queue.begin());
            }
            _lock.release();
            return static_cast<int>(copy_len);
        } else if (_type == SocketType::RAW) {
            if (_family != SocketFamily::INET || _protocol != k_protocol_icmp) {
                _lock.release();
                return -EOPNOTSUPP;
            }

            int ensure_result = ensure_onps_raw_icmp_locked();
            if (ensure_result < 0) {
                _lock.release();
                return ensure_result;
            }

            SOCKET onps_socket = _onps_socket;
            bool nonblocking = is_nonblocking_request(flags);
            uint64 timeout_us = 0;
            bool has_timeout = socket_timeout_to_usec(_recv_timeout_sec, _recv_timeout_usec, timeout_us) &&
                               timeout_us > 0;
            INT wait_secs = 0;
            if (nonblocking) {
                // onps 的 0 表示永久阻塞，raw ICMP 暂用 1 秒轮询避免非阻塞调用卡死。
                wait_secs = 1;
            } else if (has_timeout) {
                wait_secs = static_cast<INT>(_recv_timeout_sec + (_recv_timeout_usec > 0 ? 1 : 0));
                if (wait_secs <= 0) {
                    wait_secs = 1;
                }
            }
            _lock.release();

            UCHAR *icmp_packet = nullptr;
            UINT from_ip = 0;
            UCHAR ttl = k_default_ip_ttl;
            UCHAR icmp_type = 0;
            UCHAR icmp_code = 0;
            EN_ONPSERR error = ERRNO;
            INT received = onps_input_recv_icmp(static_cast<INT>(onps_socket),
                                                &icmp_packet,
                                                &from_ip,
                                                &ttl,
                                                &icmp_type,
                                                &icmp_code,
                                                wait_secs,
                                                &error);
            if (received == 0) {
                return (nonblocking || has_timeout) ? -EAGAIN : 0;
            }
            if (received < 0 || icmp_packet == nullptr) {
                int mapped = onps_error_to_errno(error);
                return mapped == 0 ? -EIO : mapped;
            }

            UINT local_ip = route_get_netif_ip(from_ip);
            if (local_ip == 0) {
                local_ip = inet_addr("10.0.2.15");
            }

            raw_ipv4_header ip_header;
            build_raw_ipv4_header(ip_header,
                                  from_ip,
                                  local_ip,
                                  ttl,
                                  static_cast<uint16_t>(received));

            size_t total_len = sizeof(ip_header) + static_cast<size_t>(received);
            size_t copy_len = eastl::min(len, total_len);
            size_t header_copy = eastl::min(copy_len, sizeof(ip_header));
            memcpy(data, &ip_header, header_copy);
            if (copy_len > sizeof(ip_header)) {
                memcpy(data + sizeof(ip_header),
                       icmp_packet,
                       copy_len - sizeof(ip_header));
            }

            if (src_addr && addrlen && *addrlen > 0) {
                struct sockaddr_in from{};
                from.sin_family = AF_INET;
                from.sin_addr = from_ip;
                from.sin_port = 0;
                socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(from)));
                memcpy(src_addr, &from, copy_addr_len);
                *addrlen = sizeof(from);
            }
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

        SOCKET onps_close_socket = INVALID_SOCKET;
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
                if (_onps_socket != INVALID_SOCKET) {
                    onps_close_socket = _onps_socket;
                    _onps_socket = INVALID_SOCKET;
                    _onps_active = false;
                    _onps_bound = false;
                    _onps_listening = false;
                }
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

        if (onps_close_socket != INVALID_SOCKET) {
            close_onps_handle(_type, onps_close_socket);
        }

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
            if (is_receive_timeout_option(optname) || is_send_timeout_option(optname)) {
                if (optlen < sizeof(socket_timeval)) {
                    _lock.release();
                    return -EINVAL;
                }

                socket_timeval timeout{};
                memcpy(&timeout, optval, sizeof(timeout));
                uint64 timeout_us = 0;
                if (!socket_timeout_to_usec(timeout.tv_sec, timeout.tv_usec, timeout_us)) {
                    _lock.release();
                    return -EDOM;
                }

                // 64 位 Linux 上 musl/glibc 仍会使用 OLD 编号 20/21；
                // 同时接受 NEW 编号，避免后续 time64 ABI 测例再落到 ENOPROTOOPT。
                if (is_receive_timeout_option(optname)) {
                    _recv_timeout_sec = timeout.tv_sec;
                    _recv_timeout_usec = timeout.tv_usec;
                } else {
                    _send_timeout_sec = timeout.tv_sec;
                    _send_timeout_usec = timeout.tv_usec;
                }
                _lock.release();
                return 0;
            }

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
                case SO_DONTROUTE:
                case SO_BROADCAST:
                case SO_OOBINLINE:
                case SO_SNDBUF:
                case SO_RCVBUF:
                case SO_SNDBUFFORCE:
                case SO_RCVBUFFORCE:
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
        else if (level == k_protocol_ipv6) {
            // AF_INET6 在 loopback 初版中只作为双栈监听兼容入口，IPV6_V6ONLY
            // 等选项不改变底层端口表行为，按 no-op 接受以兼容 iperf3 初始化。
            if (_family != SocketFamily::INET6 || optlen < sizeof(int)) {
                _lock.release();
                return -ENOPROTOOPT;
            }
            _lock.release();
            return 0;
        }
        else if (level == k_protocol_ip) {
            // IP_RECVERR 只影响真实 IP 错误队列；当前 loopback 后端没有异步
            // ICMP 错误来源，按 no-op 接受可兼容 netperf 的初始化路径。
            if (optname == k_ip_recverr && optlen >= sizeof(int)) {
                _lock.release();
                return 0;
            }
            _lock.release();
            return -ENOPROTOOPT;
        }
        else if (level == k_protocol_udp) {
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

            if (is_receive_timeout_option(optname)) {
                int result = copy_socket_timeval_option(optval, optlen,
                                                        _recv_timeout_sec,
                                                        _recv_timeout_usec);
                _lock.release();
                return result;
            }

            if (is_send_timeout_option(optname)) {
                int result = copy_socket_timeval_option(optval, optlen,
                                                        _send_timeout_sec,
                                                        _send_timeout_usec);
                _lock.release();
                return result;
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
                case SO_DONTROUTE:
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
        else if (level == k_protocol_ipv6) {
            if (_family != SocketFamily::INET6 || *optlen < sizeof(int)) {
                _lock.release();
                return -ENOPROTOOPT;
            }
            *static_cast<int*>(optval) = 0;
            *optlen = sizeof(int);
            _lock.release();
            return 0;
        }
        else if (level == k_protocol_tcp) {
            if (_type != SocketType::TCP) {
                _lock.release();
                return -ENOPROTOOPT;
            }

            if (optname == k_tcp_info) {
                // netperf 会读取 TCP_INFO 做统计。loopback 后端没有真实 RTT/拥塞窗口，
                // 但应按 Linux ABI 返回一块可读结构，并至少填出 tcpi_state。
                unsigned char state = 7; // TCP_CLOSE
                if (_state == SocketState::CONNECTED && !_peer_closed) {
                    state = 1; // TCP_ESTABLISHED
                } else if (_state == SocketState::LISTENING) {
                    state = 10; // TCP_LISTEN
                }
                memset(optval, 0, *optlen);
                if (*optlen > 0) {
                    static_cast<unsigned char *>(optval)[0] = state;
                }
                _lock.release();
                return 0;
            }

            if (optname == k_tcp_congestion) {
                static const char congestion[] = "cubic";
                socklen_t copy_len = eastl::min(*optlen, static_cast<socklen_t>(sizeof(congestion)));
                if (copy_len > 0) {
                    memcpy(optval, congestion, copy_len);
                }
                *optlen = copy_len;
                _lock.release();
                return 0;
            }

            int value = 0;
            switch (optname) {
                case k_tcp_maxseg:
                    value = k_tcp_default_maxseg;
                    break;
                case k_tcp_keepidle:
                    value = 7200;
                    break;
                case k_tcp_keepintvl:
                    value = 75;
                    break;
                case k_tcp_keepcnt:
                    value = 9;
                    break;
                case k_tcp_nodelay:
                case k_tcp_cork:
                case k_tcp_syncnt:
                case k_tcp_linger2:
                case k_tcp_defer_accept:
                case k_tcp_window_clamp:
                case k_tcp_quickack:
                case k_tcp_user_timeout:
                    value = 0;
                    break;
                default:
                    _lock.release();
                    return -ENOPROTOOPT;
            }

            int result = copy_socket_int_option(optval, optlen, value);
            _lock.release();
            return result;
        }
        else if (level == k_protocol_ip) {
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
        if (_onps_socket != INVALID_SOCKET) {
            bool keep_any_addr = _onps_bound && _loopback_registered && _local_addr.sin_addr == 0;
            refresh_onps_local_addr(_onps_socket, _local_addr);
            if (keep_any_addr) {
                _local_addr.sin_addr = 0;
            }
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

    int socket_file::ensure_onps_socket_locked()
    {
        if (_onps_socket != INVALID_SOCKET)
        {
            return 0;
        }
        if (!can_use_onps_socket(_family, _type))
        {
            return -ENETUNREACH;
        }

        EN_ONPSERR error = ERRNO;
        SOCKET socket = ::socket(AF_INET, onps_socket_type(_type), 0, &error);
        if (socket == INVALID_SOCKET)
        {
            return onps_error_to_errno(error);
        }
        _onps_socket = socket;
        return 0;
    }

    int socket_file::ensure_onps_raw_icmp_locked()
    {
        if (_onps_socket != INVALID_SOCKET)
        {
            return 0;
        }
        if (!can_use_onps_raw_icmp(_family, _type, _protocol))
        {
            return -ENETUNREACH;
        }

        EN_ONPSERR error = ERRNO;
        INT input = onps_input_new(IPPROTO_ICMP, &error);
        if (input < 0)
        {
            return onps_error_to_errno(error);
        }

        _onps_socket = static_cast<SOCKET>(input);
        _onps_active = true;
        _onps_bound = true;
        if (_state == SocketState::CREATED)
        {
            _state = SocketState::BOUND;
        }
        return 0;
    }

    int socket_file::bind_onps_locked(const struct sockaddr_in &addr)
    {
        if (_onps_bound)
        {
            return 0;
        }

        int ensure_result = ensure_onps_socket_locked();
        if (ensure_result < 0)
        {
            return ensure_result;
        }

        char ip[16];
        const char *ip_arg = nullptr;
        if (addr.sin_addr != 0)
        {
            ipv4_to_string(addr.sin_addr, ip);
            ip_arg = ip;
        }

        USHORT host_port = socket_port_to_host(addr.sin_port);
        if (::bind(_onps_socket, ip_arg, host_port) != 0)
        {
            return onps_last_errno(_onps_socket);
        }

        _onps_bound = true;
        refresh_onps_local_addr(_onps_socket, _local_addr);
        if (addr.sin_addr == 0)
        {
            _local_addr.sin_addr = 0;
        }
        return 0;
    }

    int socket_file::ensure_onps_udp_source_locked(uint32 dest_addr)
    {
        if (_type != SocketType::UDP || _onps_socket == INVALID_SOCKET)
        {
            return 0;
        }

        EN_ONPSERR error = ERRNO;
        PST_TCPUDP_HANDLE handle = nullptr;
        if (!onps_input_get(static_cast<INT>(_onps_socket), IOPT_GETTCPUDPADDR, &handle, &error) ||
            handle == nullptr)
        {
            return onps_error_to_errno(error);
        }

        if (handle->stSockAddr.saddr_ipv4 != 0)
        {
            return 0;
        }

        // UDP bind(0.0.0.0:port) 后 ONPS 已有端口但没有源 IP；
        // 发送到外部地址前必须按路由补齐源 IP，否则 IP 层会拒绝源地址不一致。
        UINT source_ip = route_get_netif_ip(dest_addr);
        if (source_ip == 0)
        {
            return -ENETUNREACH;
        }

        ST_TCPUDP_HANDLE updated = *handle;
        updated.stSockAddr.saddr_ipv4 = source_ip;
        if (!onps_input_set(static_cast<INT>(_onps_socket), IOPT_SETTCPUDPADDR, &updated, &error))
        {
            return onps_error_to_errno(error);
        }
        return 0;
    }

    int socket_file::configure_onps_recv_timeout_locked(int flags)
    {
        if (_onps_socket == INVALID_SOCKET)
        {
            return -ENOTCONN;
        }

        EN_ONPSERR error = ERRNO;
        CHAR timeout = onps_recv_timeout_seconds(is_nonblocking_request(flags),
                                                 _recv_timeout_sec,
                                                 _recv_timeout_usec);
        if (!socket_set_rcv_timeout(_onps_socket, timeout, &error))
        {
            return onps_error_to_errno(error);
        }
        return 0;
    }

    int socket_file::append_pending_send_locked(const uint8_t *data, size_t len,
                                                const struct sockaddr_in *dest_addr)
    {
        if (len > 0 && data == nullptr) {
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
        if (len > 0) {
            memcpy(_send_buffer.data() + old_size, data, len);
        }
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
        if (_family != SocketFamily::INET && _family != SocketFamily::INET6)
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

    int socket_file::enqueue_stream_data_to_peer(socket_file *peer, const uint8_t *data,
                                                 size_t len, bool nonblocking)
    {
        if (peer == nullptr)
        {
            return -EPIPE;
        }
        if (data == nullptr)
        {
            return -EFAULT;
        }
        if (len == 0)
        {
            return 0;
        }

        size_t queued = 0;
        while (queued < len)
        {
            peer->_lock.acquire();
            while (peer->_recv_buffer.size() >= k_tcp_recv_buffer_max_bytes &&
                   !peer->_read_shutdown && peer->_state != SocketState::CLOSED)
            {
                if (nonblocking)
                {
                    peer->_lock.release();
                    return queued > 0 ? static_cast<int>(queued) : -EAGAIN;
                }
                proc::k_pm.sleep(&peer->_recv_buffer, &peer->_lock);
            }

            if (peer->_read_shutdown || peer->_state == SocketState::CLOSED)
            {
                peer->_lock.release();
                return queued > 0 ? static_cast<int>(queued) : -EPIPE;
            }

            size_t used = peer->_recv_buffer.size();
            size_t space = used < k_tcp_recv_buffer_max_bytes
                               ? k_tcp_recv_buffer_max_bytes - used
                               : 0;
            if (space == 0)
            {
                peer->_lock.release();
                continue;
            }

            size_t chunk = eastl::min(len - queued, space);
            size_t old_size = peer->_recv_buffer.size();
            peer->_recv_buffer.resize(old_size + chunk);
            memcpy(peer->_recv_buffer.data() + old_size, data + queued, chunk);
            queued += chunk;

            // 新数据入队唤醒读端；同一等待点也被读端用于释放空间后的写端唤醒。
            proc::k_pm.wakeup(&peer->_recv_buffer);
            peer->_lock.release();

            if (nonblocking)
            {
                break;
            }
        }

        return static_cast<int>(queued);
    }

    int socket_file::enqueue_stream_data(const uint8_t *data, size_t len)
    {
        if (_peer == nullptr || _peer_closed)
        {
            return -EPIPE;
        }

        return enqueue_stream_data_to_peer(_peer, data, len, !_blocking);
    }

    int socket_file::enqueue_datagram(const struct sockaddr_in *src_addr, const uint8_t *data, size_t len)
    {
        if (_read_shutdown || _state == SocketState::CLOSED)
        {
            return -EPIPE;
        }

        // loopback UDP 也必须像真实内核一样有接收队列上限。iperf 的
        // 1000G 发送目标会远超 demo 内核处理速度；队列满时丢弃新包，
        // sendto 仍按 UDP 语义返回成功，避免无限分配内核内存。
        if (_datagram_queue.size() >= k_udp_queue_max_packets ||
            _datagram_queue_bytes + len > k_udp_queue_max_bytes)
        {
            return static_cast<int>(len);
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
        _datagram_queue_bytes += len;
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

        if (_family == SocketFamily::INET6 && addrlen < sizeof(struct sockaddr_in6)) {
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

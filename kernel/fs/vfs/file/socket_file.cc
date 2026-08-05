#include "fs/vfs/file/socket_file.hh"
#include "fs/vfs/ops.hh"
#include "fs/vfs/vfs_utils.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "proc/signal.hh"
#include "tm/timer_manager.hh"
#include <errno.h>
#include "fs/vfs/virtual_fs.hh"
#include "net/f7ly_network.hh"
#include "onps.hh"
#include "ip/tcp_link.hh"
#include "netif/netif.hh"

namespace fs
{
    namespace
    {
        // loopback/AF_UNIX 这两类“本机通信”不经过真实网卡。
        // 为了让 bind/connect 能按端口或路径找到对端，内核维护两张很小的全局登记表。
        constexpr int k_loopback_binding_max = 256;
        // 127.0.0.1 在本代码中按小端内存里的网络字节序保存，所以值看起来是 0x0100007f。
        constexpr uint32 k_loopback_addr = 0x0100007f; // 127.0.0.1 的网络字节序整数表示
        // 自动分配本地端口时使用的临时端口范围。
        constexpr uint16 k_ephemeral_port_start = 20000;
        constexpr uint16 k_ephemeral_port_end = 60999;
        // 协议号常量：和 Linux/IP 协议号保持一致，setsockopt 与 raw socket 会用到。
        constexpr int k_protocol_ip = 0;
        constexpr int k_protocol_icmp = 1;
        constexpr int k_protocol_tcp = 6;
        constexpr int k_protocol_udp = 17;
        constexpr int k_protocol_ipv6 = 41;
        constexpr uint8 k_default_ip_ttl = 64;
        constexpr size_t k_ipv4_header_len = 20;
        constexpr int k_ip_tos = 1;
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
            // 保存网络字节序端口，直接和 sockaddr_in.sin_port 比较，减少反复转换。
            uint16 port = 0; // 始终保存 sockaddr_in.sin_port 的网络字节序值
            SocketType type = SocketType::TCP;
            // 指向已经 bind 的 socket_file；connect/sendto 会通过它找到目标 socket。
            socket_file *socket = nullptr;
        };

        struct unix_binding
        {
            bool used = false;
            // AF_UNIX pathname socket 的绝对路径，例如 /tmp/sock。
            eastl::string path;
            socket_file *socket = nullptr;
        };

        // loopback 表和 Unix 表是全局共享结构，必须用自旋锁保护。
        SpinLock g_loopback_lock;
        bool g_loopback_ready = false;
        uint16 g_next_ephemeral_port = k_ephemeral_port_start;
        loopback_binding g_loopback_bindings[k_loopback_binding_max];

        SpinLock g_unix_lock;
        bool g_unix_ready = false;
        unix_binding g_unix_bindings[k_unix_binding_max];

        SpinLock g_socket_registry_lock;
        eastl::atomic<uint32> g_socket_registry_state{0};
        socket_file *g_socket_registry_head = nullptr;

        uint16 to_network_u16(uint16 value)
        {
            // 本内核运行在小端架构时，端口需要在主机字节序和网络字节序之间互换。
            return static_cast<uint16>(((value & 0x00ff) << 8) | ((value & 0xff00) >> 8));
        }

        int copy_socket_int_option(void *optval, socklen_t *optlen, int value)
        {
            // getsockopt 返回 int 型选项时的通用小工具：先检查用户缓冲区够不够。
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
            // timeval 语义要求 tv_usec 在 [0, 1000000)；负数也非法。
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
            // 统一从内核时间管理器取当前时间，供 SO_RCVTIMEO 计算 deadline。
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
            // type 低三位才是真正的 SOCK_STREAM/SOCK_DGRAM/SOCK_RAW。
            // 高位的 CLOEXEC/NONBLOCK 已在 syscall 层拆掉，这里再防御性归一化。
            return type & 0b111;
        }

        bool is_loopback_or_any(uint32 addr)
        {
            // 0.0.0.0 表示 INADDR_ANY；127.0.0.1 有两种字节序表示都兼容。
            return addr == 0 || addr == k_loopback_addr || addr == 0x7f000001;
        }

        bool is_valid_inet_bind_addr(uint32 addr)
        {
            if (is_loopback_or_any(addr))
            {
                return true;
            }
            return net::is_network_stack_ready() && netif_get_by_ip(addr, FALSE) != nullptr;
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
            // 当前没有完整 IPv6 协议栈，只把 ::、::1、IPv4-mapped IPv6 映射到 IPv4 loopback。
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
            // SpinLock 需要显式 init；懒初始化避免早期静态初始化顺序问题。
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
            // 按协议类型和端口查找已 bind 的本机 socket。
            // TCP 和 UDP 即使端口相同也属于不同命名空间。
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
            // bind 端口时登记到全局表；重复绑定大多返回 EADDRINUSE。
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
            // socket 析构或关闭时反注册，避免端口永久占用。
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
            // 自动 bind 时循环寻找一个未占用的临时端口。
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
            // sockaddr_un.sun_path 是以 NUL 结尾的路径；这里转成 eastl::string。
            eastl::string path;
            for (size_t i = 0; i < sizeof(addr.sun_path) && addr.sun_path[i] != '\0'; ++i)
            {
                path += addr.sun_path[i];
            }
            return path;
        }

        eastl::string abstract_unix_key_from_sockaddr(const struct sockaddr_un &addr, socklen_t addrlen)
        {
            constexpr socklen_t prefix_len =
                static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path));
            static constexpr char hex[] = "0123456789abcdef";
            eastl::string key = "abstract:";
            socklen_t path_len = addrlen > prefix_len ? addrlen - prefix_len : 0;
            if (path_len > sizeof(addr.sun_path))
            {
                path_len = sizeof(addr.sun_path);
            }
            for (socklen_t i = 0; i < path_len; ++i)
            {
                uint8 byte = static_cast<uint8>(addr.sun_path[i]);
                key += hex[(byte >> 4) & 0xf];
                key += hex[byte & 0xf];
            }
            return key;
        }

        eastl::string absolute_unix_path(const eastl::string &path)
        {
            // AF_UNIX pathname socket 按当前进程 cwd 解析相对路径，保证全局表里保存绝对路径。
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            const char *cwd = p != nullptr ? p->_cwd_name.c_str() : "/";
            return get_absolute_path(path.c_str(), cwd);
        }

        eastl::string unix_binding_key_from_sockaddr(const struct sockaddr_un &addr,
                                                     socklen_t addrlen,
                                                     bool &is_abstract)
        {
            is_abstract = addr.sun_path[0] == '\0';
            if (is_abstract)
            {
                return abstract_unix_key_from_sockaddr(addr, addrlen);
            }

            eastl::string relative_path = unix_path_from_sockaddr(addr);
            if (relative_path.empty())
            {
                return {};
            }
            return absolute_unix_path(relative_path);
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
            // 只有 IPv4 TCP/UDP 且网络栈已初始化时，才能走真实 ONPS 协议栈。
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
            // 目标不是 127.0.0.1/0.0.0.0 时，才尝试从 ONPS + VirtIO 网卡发出去。
            return can_use_onps_socket(family, type) && !is_loopback_or_any(addr);
        }

        int onps_error_to_errno(EN_ONPSERR error)
        {
            // ONPS 有自己的一套错误码；syscall 必须返回 Linux errno 风格的负数。
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
            // 从 ONPS socket 取最近错误，并转成 Linux errno。
            EN_ONPSERR error = socket_get_last_error_code(socket);
            int result = onps_error_to_errno(error);
            return result == 0 ? -EIO : result;
        }

        bool onps_tcp_recv_reached_eof(SOCKET socket)
        {
            // ONPS recv 返回 0 时需要进一步判断是真 EOF 还是暂时没数据。
            EN_ONPSERR error = socket_get_last_error_code(socket);
            if (error == ERRTCPCONNCLOSED) {
                return true;
            }

            EN_TCPLINKSTATE state = TLSINVALID;
            error = ERRNO;
            if (!onps_input_get(static_cast<INT>(socket), IOPT_GETTCPLINKSTATE, &state, &error)) {
                return false;
            }

            return state == TLSFINWAIT1 || state == TLSFINWAIT2 ||
                   state == TLSCLOSING || state == TLSTIMEWAIT ||
                   state == TLSCLOSED;
        }

        bool onps_tcp_link_state(SOCKET socket, EN_TCPLINKSTATE &state)
        {
            EN_ONPSERR error = ERRNO;
            return onps_input_get(static_cast<INT>(socket), IOPT_GETTCPLINKSTATE, &state, &error);
        }

        bool onps_tcp_connect_pending(EN_TCPLINKSTATE state)
        {
            return state == TLSSYNSENT || state == TLSRCVEDSYNACK;
        }

        bool onps_tcp_listener_has_pending(SOCKET socket)
        {
            if (socket == INVALID_SOCKET)
            {
                return false;
            }

            EN_ONPSERR error = ERRNO;
            PST_INPUTATTACH_TCPSRV attach = nullptr;
            if (!onps_input_get(static_cast<INT>(socket), IOPT_GETATTACH, &attach, &error) ||
                attach == nullptr)
            {
                return false;
            }

            return attach->usBacklogCnt > 0;
        }

        int onps_tcp_connect_so_error(SOCKET socket)
        {
            // 非阻塞 connect 完成后，用户态通常用 getsockopt(SO_ERROR) 查询最终结果。
            EN_TCPLINKSTATE state = TLSINVALID;
            if (!onps_tcp_link_state(socket, state))
            {
                return -onps_last_errno(socket);
            }

            switch (state)
            {
            case TLSCONNECTED:
                return 0;
            case TLSINIT:
            case TLSSYNSENT:
            case TLSRCVEDSYNACK:
                return EINPROGRESS;
            case TLSACKTIMEOUT:
                return ETIMEDOUT;
            case TLSRESET:
                return ECONNRESET;
            case TLSSYNACKACKSENTFAILED:
                return EIO;
            default:
                return -onps_last_errno(socket);
            }
        }

        int onps_socket_type(SocketType type)
        {
            // socket_file 的枚举类型转换成 ONPS/BSD socket API 接受的类型。
            return type == SocketType::TCP ? SOCK_STREAM : SOCK_DGRAM;
        }

        void close_onps_handle(SocketType type, SOCKET socket)
        {
            // TCP/UDP 走 ONPS close；RAW ICMP 是 onps_input_new 创建的 input，需要用 input_free。
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
            // ONPS 可能在 bind/connect/sendto 时自动分配本地端口或源 IP；
            // socket_file 要同步这些信息，getsockname/poll 后续才看到正确状态。
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
        , _peer_write_shutdown(false)
        , _pending_send_has_addr(false)
        , _recv_timeout_sec(0)
        , _recv_timeout_usec(0)
        , _send_timeout_sec(0)
        , _send_timeout_usec(0)
        , _ip_tos(0)
    {
        // socket_file 是一个 VFS file，因此 stat 类型也要标成 socket，fstat 才能看到 S_IFSOCK。
        new(&_stat) Kstat(FT_SOCKET);
        // 地址结构先清零；后续 bind/connect/listen 再逐步填 local/remote。
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        memset(&_pending_send_addr, 0, sizeof(_pending_send_addr));
        memset(&_local_unix_addr, 0, sizeof(_local_unix_addr));
        memset(&_remote_unix_addr, 0, sizeof(_remote_unix_addr));
        // 默认可读可写；O_NONBLOCK/CLOEXEC 会在 syscall 层补到 flags/fd 表。
        lwext4_file_struct.flags = O_RDWR;
        // 每个 socket_file 独立一把锁，保护状态机、缓冲区和 peer 指针。
        _lock.init("socket_lock");
        // file 基类用引用计数管理生命周期；创建后持有一个引用。
        dup();
        initialize_proc_registry();
        g_socket_registry_lock.acquire();
        _proc_registry_next = g_socket_registry_head;
        g_socket_registry_head = this;
        g_socket_registry_lock.release();
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
        , _peer_write_shutdown(false)
        , _pending_send_has_addr(false)
        , _recv_timeout_sec(0)
        , _recv_timeout_usec(0)
        , _send_timeout_sec(0)
        , _send_timeout_usec(0)
        , _ip_tos(0)
    {
        new(&_stat) Kstat(FT_SOCKET);
        memset(&_local_addr, 0, sizeof(_local_addr));
        memset(&_remote_addr, 0, sizeof(_remote_addr));
        memset(&_local_unix_addr, 0, sizeof(_local_unix_addr));
        memset(&_remote_unix_addr, 0, sizeof(_remote_unix_addr));
        lwext4_file_struct.flags = O_RDWR;
        _lock.init("socket_lock");
        dup();
        initialize_proc_registry();
        g_socket_registry_lock.acquire();
        _proc_registry_next = g_socket_registry_head;
        g_socket_registry_head = this;
        g_socket_registry_lock.release();
    }

    socket_file::~socket_file()
    {
        // 必须先从快照登记表摘除，再取得对象锁；快照使用“登记锁 -> 对象锁”
        // 的固定顺序，这样析构和 /proc/net/tcp 并发时不会死锁或悬空。
        g_socket_registry_lock.acquire();
        socket_file **link = &g_socket_registry_head;
        while (*link != nullptr && *link != this)
        {
            link = &(*link)->_proc_registry_next;
        }
        if (*link == this)
        {
            *link = _proc_registry_next;
        }
        _proc_registry_next = nullptr;
        g_socket_registry_lock.release();
        // 析构时要先把自身状态切到 CLOSED，并唤醒所有可能睡在这个 socket 上的线程。
        _lock.acquire();
        _state = SocketState::CLOSED;
        _send_buffer.clear();
        _pending_send_has_addr = false;
        SOCKET onps_socket = _onps_socket;
        _onps_socket = INVALID_SOCKET;
        _onps_active = false;
        _onps_bound = false;
        _onps_listening = false;
        // accept/recv/recvfrom 可能阻塞在这些等待点，关闭时必须唤醒它们返回错误或 EOF。
        proc::k_pm.wakeup(&_pending_connections);
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
        _lock.release();

        if (onps_socket != INVALID_SOCKET)
        {
            // 真实网络 socket 还需要释放 ONPS 协议栈中的句柄。
            close_onps_handle(_type, onps_socket);
        }

        ensure_loopback_table();
        if (_loopback_registered)
        {
            // 如果曾经 bind 到 loopback 端口表，关闭时必须释放端口。
            g_loopback_lock.acquire();
            unregister_loopback_binding(_type, _local_addr.sin_port, this);
            g_loopback_lock.release();
            _loopback_registered = false;
        }

        ensure_unix_table();
        if (_unix_registered)
        {
            // 如果曾经 bind 到 AF_UNIX 路径表，关闭时必须释放路径登记。
            g_unix_lock.acquire();
            unregister_unix_binding(_unix_path, this);
            g_unix_lock.release();
            _unix_registered = false;
        }

        // fd 关闭时通知对端，阻塞在 recv/accept 的进程需要被唤醒。
        if (_peer != nullptr)
        {
            // loopback/AF_UNIX 对端通过 peer 指针感知 EOF。
            _peer->_lock.acquire();
            _peer->mark_stream_peer_closed_locked(this);
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

    void socket_file::initialize_proc_registry()
    {
        constexpr uint32 k_uninitialized = 0;
        constexpr uint32 k_initializing = 1;
        constexpr uint32 k_ready = 2;
        if (g_socket_registry_state.load(eastl::memory_order_acquire) == k_ready)
        {
            return;
        }

        uint32 expected = k_uninitialized;
        if (g_socket_registry_state.compare_exchange_strong(
                expected, k_initializing, eastl::memory_order_acq_rel))
        {
            g_socket_registry_lock.init("socket_proc_registry");
            g_socket_registry_head = nullptr;
            g_socket_registry_state.store(k_ready, eastl::memory_order_release);
            return;
        }
        while (g_socket_registry_state.load(eastl::memory_order_acquire) != k_ready)
        {
            asm volatile("nop");
        }
    }

    eastl::string socket_file::generate_tcp_proc_snapshot(bool ipv6)
    {
        initialize_proc_registry();
        eastl::string result =
            "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
        int slot = 0;

        g_socket_registry_lock.acquire();
        for (socket_file *socket = g_socket_registry_head;
             socket != nullptr;
             socket = socket->_proc_registry_next)
        {
            if (socket == nullptr)
            {
                continue;
            }

            socket->_lock.acquire();
            const bool family_matches = ipv6
                                            ? socket->_family == SocketFamily::INET6
                                            : socket->_family == SocketFamily::INET;
            if (!family_matches || socket->_type != SocketType::TCP ||
                socket->_state == SocketState::CREATED ||
                socket->_state == SocketState::CLOSED)
            {
                socket->_lock.release();
                continue;
            }

            uint state = 0x07; // TCP_CLOSE：已 bind 但尚未 listen/connect。
            if (socket->_state == SocketState::LISTENING)
            {
                state = 0x0a;
            }
            else if (socket->_state == SocketState::CONNECTING)
            {
                state = 0x02;
            }
            else if (socket->_state == SocketState::CONNECTED)
            {
                state = 0x01;
            }

            // 非阻塞 connect 的用户态状态会保持 CONNECTING，直到 poll/getsockopt
            // 消费完成事件；proc 快照应直接读取协议栈，避免已经握手成功仍报告 SYN_SENT。
            if (socket->_onps_socket != INVALID_SOCKET &&
                socket->_state != SocketState::LISTENING)
            {
                EN_TCPLINKSTATE link_state = TLSINVALID;
                if (onps_tcp_link_state(socket->_onps_socket, link_state))
                {
                    switch (link_state)
                    {
                    case TLSCONNECTED:
                        state = 0x01; // TCP_ESTABLISHED
                        break;
                    case TLSSYNSENT:
                    case TLSRCVEDSYNACK:
                        state = 0x02; // TCP_SYN_SENT
                        break;
                    case TLSRCVEDSYN:
                    case TLSSYNACKSENT:
                        state = 0x03; // TCP_SYN_RECV
                        break;
                    case TLSFINWAIT1:
                        state = 0x04;
                        break;
                    case TLSFINWAIT2:
                        state = 0x05;
                        break;
                    case TLSTIMEWAIT:
                        state = 0x06;
                        break;
                    case TLSCLOSING:
                        state = 0x0b;
                        break;
                    default:
                        break;
                    }
                }
            }

            const uint16 local_port = to_network_u16(socket->_local_addr.sin_port);
            const uint16 remote_port = to_network_u16(socket->_remote_addr.sin_port);
            char line[256];
            if (ipv6)
            {
                // socket 后端目前把 ::/::1/IPv4-mapped IPv6 统一映射为内部 IPv4
                // endpoint；proc 视图以 IPv4-mapped IPv6 形式无损公开端口和地址。
                char local_address[33];
                char remote_address[33];
                if (socket->_local_addr.sin_addr == 0)
                {
                    snprintf(local_address, sizeof(local_address), "%032x", 0);
                }
                else
                {
                    snprintf(local_address, sizeof(local_address),
                             "0000000000000000FFFF0000%08X", socket->_local_addr.sin_addr);
                }
                if (socket->_remote_addr.sin_addr == 0)
                {
                    snprintf(remote_address, sizeof(remote_address), "%032x", 0);
                }
                else
                {
                    snprintf(remote_address, sizeof(remote_address),
                             "0000000000000000FFFF0000%08X", socket->_remote_addr.sin_addr);
                }
                snprintf(line, sizeof(line),
                         "%4d: %s:%04X %s:%04X %02X 00000000:00000000 00:00000000 00000000 0 0 %lu\n",
                         slot++, local_address, local_port, remote_address, remote_port,
                         state, reinterpret_cast<unsigned long>(socket));
            }
            else
            {
                snprintf(line, sizeof(line),
                         "%4d: %08X:%04X %08X:%04X %02X 00000000:00000000 00:00000000 00000000 0 0 %lu\n",
                         slot++, socket->_local_addr.sin_addr, local_port,
                         socket->_remote_addr.sin_addr, remote_port, state,
                         reinterpret_cast<unsigned long>(socket));
            }
            result += line;
            socket->_lock.release();
        }
        g_socket_registry_lock.release();
        return result;
    }

    long socket_file::read(uint64 buf, size_t len, long off, bool upgrade)
    {
        // socket 没有文件偏移概念；read(fd) 直接等价于 recv(fd, ..., 0)。
        return recv((void*)buf, len, 0);
    }

    long socket_file::write(uint64 buf, size_t len, long off, bool upgrade)
    {
        // write(fd) 直接等价于 send(fd, ..., 0)。
        return send((const void*)buf, len, 0);
    }

    bool socket_file::read_ready()
    {
        // poll/select/epoll 判断“可读”时会走这里。
        // 可读不只代表有数据，也可能代表对端关闭后读会立刻返回 EOF。
        _lock.acquire();
        bool result;
        switch (_state) {
            case SocketState::CONNECTED:
                if (_onps_active)
                {
                    // ONPS 真实网络路径：问协议栈是否已经有待读数据。
                    if (_read_shutdown)
                    {
                        result = true;
                        break;
                    }
                    if (_type == SocketType::UDP)
                    {
                        result = udp_read_ready_locked();
                        break;
                    }
                    if (_type == SocketType::TCP)
                    {
                        result = _onps_socket != INVALID_SOCKET &&
                                 (onps_input_has_pending_data(static_cast<INT>(_onps_socket)) ||
                                  onps_tcp_recv_reached_eof(_onps_socket));
                        break;
                    }
                    result = false;
                    break;
                }
                if (_type == SocketType::UDP)
                {
                    // 已 connect 的 loopback UDP 只需要看内核 datagram 队列。
                    // 绑定在 INADDR_ANY 上的 UDP 走 BOUND 分支，那里会同时检查 ONPS 队列。
                    result = !_datagram_queue.empty();
                }
                else
                {
                    // loopback TCP/AF_UNIX 看 stream 缓冲或 EOF 状态。
                    result = stream_read_ready_locked();
                }
                break;
            case SocketState::CONNECTING:
                result = false;
                break;
            case SocketState::LISTENING:
                // 监听 socket 可读表示 accept 不会阻塞：
                // loopback/AF_UNIX 看本地 pending 队列；ONPS TCP listener 还要确认协议栈中确实有待 accept 连接。
                result = !_pending_connections.empty() ||
                         (_onps_listening && onps_tcp_listener_has_pending(_onps_socket));
                break;
            case SocketState::BOUND:
                if (_type == SocketType::UDP)
                {
                    result = udp_read_ready_locked();
                }
                else
                {
                    result = false;
                }
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
        // poll/select/epoll 判断“可写”时会走这里。
        // 对 TCP 来说，可写还要考虑连接是否完成、对端接收缓冲是否有空间。
        _lock.acquire();
        bool result = false;
        if (_state == SocketState::CONNECTING)
        {
            // 非阻塞 ONPS TCP connect 期间，POLLOUT 用来通知连接完成或失败。
            if (_onps_active && _type == SocketType::TCP && _onps_socket != INVALID_SOCKET)
            {
                EN_TCPLINKSTATE link_state = TLSINVALID;
                if (onps_tcp_link_state(_onps_socket, link_state))
                {
                    if (link_state == TLSCONNECTED)
                    {
                        // 非阻塞 connect 的完成由 poll(POLLOUT) 观察；这里同步 socket_file 状态。
                        _state = SocketState::CONNECTED;
                        result = !_write_shutdown;
                    }
                    else if (!onps_tcp_connect_pending(link_state))
                    {
                        // 连接失败也要唤醒 poll，用户态随后通过 SO_ERROR 读取具体错误。
                        result = true;
                    }
                }
            }
        }
        else if (_state == SocketState::CONNECTED)
        {
            if (_onps_active)
            {
                // 真实网络路径暂不在这里精确检查发送队列，只要写半边没关就认为可写。
                result = !_write_shutdown;
                _lock.release();
                return result;
            }
            if (_type == SocketType::TCP)
            {
                socket_file *peer = _peer;
                bool local_ready = stream_write_open_locked();
                _lock.release();
                if (!local_ready)
                {
                    return false;
                }

                peer->_lock.acquire();
                // TCP 写就绪必须反映对端接收队列空间；否则 poll/select 会在队列已满时
                // 继续驱动写入，iperf 这类吞吐工具会把内核堆推到无限扩容。
                result = !peer->stream_receive_open_locked() ||
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
        // 2. 对端 shutdown(SHUT_WR) 或 close 后，本端会读到 EOF。
        // LTP epoll_wait05 就依赖这两类状态都能被 epoll 观察到。
        bool ready = _state == SocketState::CONNECTED &&
                     _type == SocketType::TCP &&
                     stream_read_eof_locked();
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
        // socket_file::bind 是 bind 语义真正落地的位置：
        // 1. AF_UNIX 注册路径；
        // 2. loopback IPv4/IPv6 注册本机端口；
        // 3. 非 loopback IPv4 必要时绑定 ONPS socket。
        if (!is_valid_address(addr, addrlen)) {
            return -EINVAL;
        }

        _lock.acquire();
        
        // 一个 socket 只能在 CREATED 状态 bind 一次；已经 bind/listen/connect 后不能重复 bind。
        if (_state != SocketState::CREATED) {
            _lock.release();
            return -EINVAL;
        }

        if (_family != SocketFamily::INET && _family != SocketFamily::INET6) {
            // 不是 INET/INET6，就只剩 AF_UNIX 路径。
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

            bool abstract_addr = false;
            eastl::string binding_key = unix_binding_key_from_sockaddr(local_unix_addr, addrlen, abstract_addr);
            if (binding_key.empty()) {
                _lock.release();
                return -EINVAL;
            }
            _lock.release();

            eastl::string relative_path;
            if (!abstract_addr) {
                // pathname AF_UNIX bind 要在文件系统里留下一个可 unlink 的 socket 节点；
                // binding_key 对 pathname 已经是绝对路径，可直接用于前缀检查和全局登记。
                relative_path = unix_path_from_sockaddr(local_unix_addr);
                int prefix_error = unix_path_prefix_error(binding_key);
                if (prefix_error < 0) {
                    return prefix_error;
                }

                // pathname AF_UNIX bind 在文件系统中有可 unlink 的 socket 节点；
                // recvmsg01 的 cleanup 依赖这个节点真实存在。abstract socket 属于
                // 内核命名空间，不落盘，也不参与路径前缀检查。
                int node_result = proc::k_pm.mknod(k_at_fdcwd, relative_path, S_IFSOCK | 0777, 0);
                if (node_result < 0) {
                    return node_result == -EEXIST ? -EADDRINUSE : node_result;
                }
            }

            ensure_unix_table();
            g_unix_lock.acquire();
            // 全局 AF_UNIX 表按 binding_key 登记：
            // pathname 使用绝对路径，abstract socket 使用内核命名空间里的抽象 key。
            int register_result = register_unix_binding(binding_key, this);
            g_unix_lock.release();
            if (register_result < 0) {
                if (!abstract_addr) {
                    proc::k_pm.unlink(k_at_fdcwd, relative_path, 0);
                }
                return register_result;
            }

            _lock.acquire();
            if (_state != SocketState::CREATED) {
                _lock.release();
                g_unix_lock.acquire();
                unregister_unix_binding(binding_key, this);
                g_unix_lock.release();
                if (!abstract_addr) {
                    proc::k_pm.unlink(k_at_fdcwd, relative_path, 0);
                }
                return -EINVAL;
            }
            _local_unix_addr = local_unix_addr;
            _unix_path = binding_key;
            _unix_registered = true;
            // bind 成功后进入 BOUND，后续 listen/connect/sendto 会基于这个状态继续推进。
            _state = SocketState::BOUND;
            _lock.release();
            return 0;
        }

        struct sockaddr_in local_addr;
        if (_family == SocketFamily::INET6) {
            // IPv6 当前只允许映射到 loopback IPv4；不可映射的 IPv6 地址直接不支持。
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
        if (!is_valid_inet_bind_addr(local_addr.sin_addr)) {
            _lock.release();
            return -EADDRNOTAVAIL;
        }

        bool bind_loopback = is_loopback_or_any(local_addr.sin_addr);
        // 0.0.0.0 会同时允许 loopback 和 ONPS；非 loopback 只有网络栈可用时才能 bind。
        bool bind_onps = can_use_onps_socket(_family, _type) &&
                         (local_addr.sin_addr == 0 || !bind_loopback);
        if (!bind_loopback && !bind_onps) {
            _lock.release();
            return -EADDRNOTAVAIL;
        }

        if (local_addr.sin_port == 0) {
            // 端口 0 表示让内核自动分配临时端口。
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
            // 登记到本机端口表，供 loopback connect/sendto 查找。
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
            // 真实网络路径同时绑定 ONPS socket，后续外网收发才能进入协议栈。
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
        // listen 只改变 TCP socket 的状态和接收连接队列，不直接返回新连接。
        _lock.acquire();
        
        if (_type != SocketType::TCP) {
            _lock.release();
            return -EOPNOTSUPP;
        }

        if (_state == SocketState::CREATED) {
            // Linux 允许未显式 bind 的 TCP socket listen；这里自动分配 loopback 临时端口。
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
            // 如果这个 socket 已经绑定了 ONPS 句柄，还要让 ONPS TCP server 进入监听状态。
            int onps_backlog = _backlog > TCPSRV_BACKLOG_NUM_MAX ? TCPSRV_BACKLOG_NUM_MAX : _backlog;
            if (::listen(_onps_socket, static_cast<USHORT>(onps_backlog)) != 0) {
                int result = onps_last_errno(_onps_socket);
                _lock.release();
                return result;
            }
            _onps_listening = true;
        }
        _state = SocketState::LISTENING;
        // pending_connections 保存 loopback/AF_UNIX connect 创建出来的 server-side socket。
        _pending_connections.reserve(_backlog);
        
        _lock.release();
        return 0;
    }

    int socket_file::accept(struct sockaddr *addr, socklen_t *addrlen, socket_file **accepted_socket)
    {
        // accept 返回一个新的已连接 socket_file；syscall 层随后给它分配 fd。
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
            // accept(2) 是信号可中断的阻塞系统调用。监听 INADDR_ANY 时同一个
            // socket 可能同时启用 ONPS 和 loopback，必须在选择后端前检查信号；
            // 否则空队列会反复进入 ONPS accept，netperf TCP_CRR 的 SIGALRM
            // 无法打断等待并回传最终统计。
            if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                _lock.release();
                return -EINTR;
            }
            if (_onps_listening) {
                // ONPS listener 没有走本地 pending_connections，直接调用协议栈 accept。
                SOCKET listen_socket = _onps_socket;
                bool blocking = _blocking;
                _lock.release();

                UINT client_ip = 0;
                USHORT client_port = 0;
                EN_ONPSERR error = ERRNO;
                // ONPS 的阻塞 accept 无法被内核信号路径可靠打断。netserver
                // 后台退出时如果卡在这里，shell 清理阶段就可能永远等不到它退出。
                // 统一用非阻塞探测 + 内核 sleep 轮询，让 SIGTERM/SIGALRM/close
                // 都能回到本层检查点。
                SOCKET client = ::accept(listen_socket, &client_ip, &client_port,
                                         0, &error);
                if (client != INVALID_SOCKET) {
                    // ONPS 返回的是协议栈 socket 句柄；这里包装成 F7LY 的 socket_file。
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
                    // ONPS 返回的 client_port 是主机字节序，socket_file 内部 sockaddr 要保存网络字节序。
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
                // 非阻塞探测无连接时，ONPS 可能设 ERRNO/ERRTCPRCVQUEUEEMPTY 等。
                // 只有真正的硬错误（reset/timeout/not-connected）才应立即返回；
                // 队列空、服务空闲这类"暂无连接"状态应继续 sleep 轮询。
                if (error != ERRNO) {
                    switch (error) {
                    case ERRTCPRCVQUEUEEMPTY:
                    case ERRTCPSRVEMPTY:
                    case ERRTCPBACKLOGEMPTY:
                        // 暂无可用连接，继续 sleep 轮询
                        break;
                    default: {
                        int mapped = onps_error_to_errno(error);
                        if (mapped != -EIO && mapped != -ENOBUFS) {
                            return mapped;
                        }
                    }
                    }
                }

                _lock.acquire();
                if (_state != SocketState::LISTENING) {
                    _lock.release();
                    return -EINVAL;
                }
                proc::k_pm.sleep(tmm::k_tm.get_tick_wait_channel(), &_lock);
                if (_state != SocketState::LISTENING) {
                    _lock.release();
                    return -EINVAL;
                }
                continue;
            }
            if (!_blocking) {
                // 非阻塞 listener 没有连接时立即返回 EAGAIN。
                _lock.release();
                return -EAGAIN;
            }
            // 阻塞 listener 睡在 pending 队列地址上，connect 成功或 close 会 wakeup。
            proc::k_pm.sleep(&_pending_connections, &_lock);
            if (_state != SocketState::LISTENING) {
                _lock.release();
                return -EINVAL;
            }
        }

        // 获取一个待处理的连接
        // loopback/AF_UNIX connect 会提前把 server-side socket 放进这个队列。
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
        // connect 是主动连接入口。根据 socket family 和目标地址分三条路：
        // AF_UNIX 查路径表；loopback 查本机端口表；外部 IPv4 走 ONPS。
        if (!addr || addrlen < sizeof(struct sockaddr)) {
            return -EINVAL;
        }

        _lock.acquire();
        
        // 只有新建或已 bind 未连接的 socket 能 connect。
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

            bool abstract_addr = false;
            eastl::string binding_key = unix_binding_key_from_sockaddr(remote_unix_addr, addrlen, abstract_addr);
            (void)abstract_addr;
            if (binding_key.empty()) {
                _lock.release();
                return -EINVAL;
            }

            ensure_unix_table();
            g_unix_lock.acquire();
            // connect 使用和 bind 相同的 binding_key 查表，才能同时支持 pathname 与 abstract AF_UNIX。
            unix_binding *binding = find_unix_binding(binding_key);
            socket_file *listener = binding ? binding->socket : nullptr;
            if (listener == nullptr) {
                g_unix_lock.release();
                _lock.release();
                return -ENOENT;
            }

            listener->_lock.acquire();
            // 目标必须已经 listen，并且 accept 队列还有空间。
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

            // 客户端和 server-side socket 互设 peer，之后 send/recv 都是内核内存拷贝。
            _peer = server_side;
            _peer_closed = false;
            _peer_write_shutdown = false;
            _state = SocketState::CONNECTED;
            // server-side socket 放到 listener 队列，等待 accept 取走。
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
                // 非 loopback IPv4 连接走 ONPS + VirtIO 真实网络路径。
                if (!should_route_via_onps(_family, _type, remote_addr.sin_addr)) {
                    _lock.release();
                    return -ENETUNREACH;
                }

                // 确保 ONPS socket 句柄存在；它类似外部协议栈里的 fd。
                int ensure_result = ensure_onps_socket_locked();
                if (ensure_result < 0) {
                    _lock.release();
                    return ensure_result;
                }
                if (_state == SocketState::BOUND && !_onps_bound) {
                    // 用户先 bind 再 connect 时，需要把本地地址也同步绑定到 ONPS。
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
                    // 非阻塞 TCP connect：先发起握手，返回 EINPROGRESS，让 poll/SO_ERROR 后续观察。
                    result = ::connect_nb_ext(onps_socket, &remote_addr.sin_addr, host_port);
                    if (result == 1) {
                        _lock.acquire();
                        _remote_addr = remote_addr;
                        _onps_active = true;
                        _state = SocketState::CONNECTING;
                        refresh_onps_local_addr(_onps_socket, _local_addr);
                        _lock.release();
                        return -EINPROGRESS;
                    }
                } else {
                    // 阻塞 TCP 或 UDP connect 直接调用 ONPS connect_ext。
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
                // loopback connect 如果用户没 bind，自动分配一个本地临时端口。
                int bind_result = ensure_loopback_bound_locked();
                if (bind_result < 0) {
                    _lock.release();
                    return bind_result;
                }
            }

            _remote_addr = remote_addr;

            if (_type == SocketType::UDP) {
                // UDP connect 只记录默认远端地址，不建立真正连接。
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
                // loopback TCP 按目标端口查找已经 listen 的本机 socket。
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
                    // 没有 listener 时，非阻塞或等待超时都按连接拒绝处理。
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
                // 注意 sleep 会临时释放 _lock，醒来后再重新持锁。
                proc::k_pm.sleep(tmm::k_tm.get_tick_wait_channel(), &_lock);
                if (_state != SocketState::CREATED && _state != SocketState::BOUND) {
                    _lock.release();
                    return _state == SocketState::CONNECTED ? -EISCONN : -ECONNABORTED;
                }
            }

            // 为 listener 创建一个“服务端视角”的已连接 socket，等待 accept 返回给用户态。
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

            // 当前客户端和 server-side socket 互连。
            _peer = server_side;
            _peer_closed = false;
            _peer_write_shutdown = false;
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
        // send 是 connected socket 的发送入口：
        // TCP 必须 CONNECTED；UDP 必须已经 connect 或有 _peer；RAW 不走这里。
        if (len == 0) {
            return 0;
        }
        if (!buf) {
            return -EFAULT;
        }

        _lock.acquire();

        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            // TCP 是面向连接的字节流，未连接不能发送。
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            if (_onps_active) {
                // 真实网络 TCP 交给 ONPS 发送。
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
                    // ONPS 的 send_nb 非阻塞尝试发送，返回 >0 表示实际发送字节数。
                    INT sent = ::send_nb(onps_socket, const_cast<UCHAR *>(data), request_len);
                    if (sent > 0) {
                        return sent;
                    }
                    if (sent < 0) {
                        return onps_last_errno(onps_socket);
                    }
                    if (nonblocking) {
                        // 用户要求非阻塞时，暂时发不出去就返回 EAGAIN。
                        return -EAGAIN;
                    }
                    proc::Pcb *cur = proc::k_pm.get_cur_pcb();
                    if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                        return -EINTR;
                    }
                    os_sleep_ms(1);
                }
            }
            if (!stream_write_open_locked()) {
                // 本端写半边关闭、对端关闭或没有 peer，都按管道破裂处理。
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
                // MSG_MORE 表示用户还有后续数据，本次先暂存，不立刻推给对端。
                int append_result = append_pending_send_locked(data, len, nullptr);
                _lock.release();
                return append_result;
            }

            if (!_send_buffer.empty()) {
                // 之前 MSG_MORE 暂存过数据，这次发送要先把旧数据和新数据拼起来。
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
                // 非阻塞 flush 不能先清空本端暂存再发现对端没空间，否则会丢数据。
                // 所以先在对端锁下预检查是否能完整放入。
                peer->_lock.acquire();
                bool peer_broken = !peer->stream_receive_open_locked();
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
                // 已确认可以发送后，再清空本端 MSG_MORE 暂存缓冲。
                _lock.acquire();
                _send_buffer.clear();
                _pending_send_has_addr = false;
                _lock.release();
            }

            // 只在本端锁内读取连接状态；实际入队时只持有对端锁，避免双向 send 互相等待。
            // loopback TCP 的发送本质是把字节写进 peer->_recv_buffer。
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
                size_t written = static_cast<size_t>(queued) - pending_len;
                return static_cast<int>(written);
            }
            return -EPIPE;
        } else if (_type == SocketType::UDP) {
            // UDP send 没有字节流连接；connect 后会记录默认目标地址。
            if (_write_shutdown) {
                _lock.release();
                return -EPIPE;
            }
            if (_peer != nullptr) {
                // socketpair(AF_UNIX/SOCK_DGRAM) 这类本地互连 datagram 走 peer 队列。
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
                // 未 connect 的 UDP 必须使用 sendto 指定目标地址。
                _lock.release();
                return -EDESTADDRREQ;
            }
            struct sockaddr_in remote = _remote_addr;
            _lock.release();
            // connected UDP 的 send 等价于 sendto(default_remote)。
            return sendto(buf, len, flags, reinterpret_cast<const struct sockaddr *>(&remote), sizeof(remote));
        } else {
            _lock.release();
            return -EOPNOTSUPP;
        }
    }

    int socket_file::recv(void *buf, size_t len, int flags)
    {
        // recv 是 connected socket 的接收入口；UDP 会转到 recvfrom，TCP 在这里处理字节流。
        if (len == 0) {
            return 0;
        }
        if (!buf) {
            return -EFAULT;
        }

        _lock.acquire();

        uint8_t* data = static_cast<uint8_t*>(buf);

        if (_type == SocketType::TCP) {
            // TCP 未连接不能接收。
            if (_state != SocketState::CONNECTED) {
                _lock.release();
                return -ENOTCONN;
            }
            if (_onps_active) {
                // 真实网络 TCP 接收走 ONPS。
                if (_read_shutdown) {
                    _lock.release();
                    return 0;
                }
                // 每次接收前把 O_NONBLOCK/MSG_DONTWAIT/SO_RCVTIMEO 同步到 ONPS。
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
                // ONPS recv 会把数据直接写到内核临时缓冲 data。
                INT received = ::recv(onps_socket, data, request_len);
                if (received > 0) {
                    return received;
                }
                if (received == 0) {
                    if (onps_tcp_recv_reached_eof(onps_socket)) {
                        return 0;
                    }
                    return (nonblocking || has_timeout) ? -EAGAIN : 0;
                }
                if (onps_tcp_recv_reached_eof(onps_socket)) {
                    return 0;
                }
                return onps_last_errno(onps_socket);
            }

            proc::Pcb *cur = proc::k_pm.get_cur_pcb();
            uint64 timeout_us = 0;
            bool has_timeout = socket_timeout_to_usec(_recv_timeout_sec, _recv_timeout_usec, timeout_us) &&
                               timeout_us > 0;
            uint64 deadline_us = has_timeout ? socket_now_usec() + timeout_us : 0;
            while (_recv_buffer.empty() && !stream_read_eof_locked()) {
                // loopback TCP 没数据时进入阻塞等待，直到 send/close/shutdown 唤醒。
                if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                    _lock.release();
                    return -EINTR;
                }
                if (is_nonblocking_request(flags)) {
                    // 非阻塞读没有数据时返回 EAGAIN。
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
                    // 普通阻塞读睡在 _recv_buffer 地址上；发送方入队后会 wakeup 同一地址。
                    proc::k_pm.sleep(&_recv_buffer, &_lock);
                }
            }

            if (_recv_buffer.empty()) {
                // 没有数据但已经 EOF，TCP 读返回 0。
                _lock.release();
                return 0;
            }

            // TCP 是字节流：一次 recv 最多取用户要求的 len 字节，可小于发送方单次 send。
            size_t copy_len = eastl::min(len, _recv_buffer.size());
            memcpy(data, _recv_buffer.data(), copy_len);
            if (!(flags & MSG_PEEK)) {
                // MSG_PEEK 只窥视不消费；普通 recv 要从接收缓冲移除已读字节。
                _recv_buffer.erase(_recv_buffer.begin(), _recv_buffer.begin() + copy_len);
                // 读端释放接收队列空间后唤醒阻塞写端，形成 TCP loopback 背压闭环。
                proc::k_pm.wakeup(&_recv_buffer);
            }
            _lock.release();
            return static_cast<int>(copy_len);
        } else if (_type == SocketType::UDP) {
            _lock.release();
            // UDP 需要保留源地址语义，所以统一转给 recvfrom。
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
        // sendto 是显式指定目标地址的发送入口，主要服务 UDP 和 RAW ICMP。
        if (len > 0 && !buf) {
            return -EFAULT;
        }
        if (flags & MSG_OOB) {
            return _type == SocketType::UDP ? -EOPNOTSUPP : -EINVAL;
        }

        _lock.acquire();

        const uint8_t* data = static_cast<const uint8_t*>(buf);

        if (_type == SocketType::UDP) {
            // UDP 一枚 datagram 最大不超过当前默认 socket buffer。
            if (len > k_default_socket_buffer_size) {
                _lock.release();
                return -EMSGSIZE;
            }
            struct sockaddr_in dest;
            if (dest_addr != nullptr) {
                // 用户传入目标地址时，当前路径要求完整 IPv4 sockaddr_in。
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
                // 没有显式目标地址时，只能用于 connected UDP。
                if (_state != SocketState::CONNECTED) {
                    _lock.release();
                    return -EDESTADDRREQ;
                }
                dest = _remote_addr;
            }

            bool route_onps = !is_loopback_or_any(dest.sin_addr);
            if (route_onps) {
                // 目标不是本机地址，走 ONPS + VirtIO 的真实网络路径。
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
                    // 用户先 bind 到本地端口后再 sendto 外部地址，要同步绑定 ONPS。
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
                // loopback UDP 首次发送前如果没 bind，自动分配本地临时端口。
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
                // UDP 的 MSG_MORE 表示把多次 sendto 合并成一枚 datagram。
                int append_result = append_pending_send_locked(data, len, &dest);
                _lock.release();
                return append_result;
            }

            if (!_send_buffer.empty()) {
                // 之前 MSG_MORE 暂存过数据，本次必须发往同一个目标。
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
                // ONPS sendto 接口接收点分十进制 IP 字符串和主机字节序端口。
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
                // ONPS udp_sendto() 会为未显式 bind 的 UDP input 自动分配本地端口。
                // socket 层必须同步标记已绑定，否则 poll/select 会走 loopback 队列，
                // 看不到 DNS 回包所在的 ONPS 接收队列。
                _onps_bound = true;
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
                // loopback UDP 按目标端口找接收者。
                // 如果目标是 connected UDP，要求它的 remote 正好匹配本端源地址。
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
                // 找到目标 socket 后，把整枚 datagram 入队到目标接收队列。
                target->_lock.acquire();
                target->enqueue_datagram(&src, send_data, send_len);
                target->_lock.release();
            }
            return static_cast<int>(len);
        } else if (_type == SocketType::RAW) {
            // 当前 RAW socket 只支持 IPv4 ICMP echo，用于 ping 这类测试。
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
                // RAW ICMP 当前只走 ONPS 外部网络，不支持 loopback raw。
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
            // 交给 ONPS ICMP 模块构造并发送 echo request。
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
            // TCP sendto 在已连接时退化成 send；未连接时按 Linux 常见语义返回 EPIPE。
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
        // recvfrom 是接收入口，UDP/RAW 需要返回源地址；TCP 会退化成 recv 并可选返回 peer。
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
            // UDP 必须已经 bind 或 connect 才能接收。
            if (_state != SocketState::BOUND && _state != SocketState::CONNECTED) {
                _lock.release();
                return -EINVAL;
            }

            bool onps_ready = onps_udp_read_ready_locked();
            bool has_loopback_data = !_datagram_queue.empty();
            bool onps_only = _onps_bound && !_loopback_registered;
            if (onps_ready || (onps_only && !has_loopback_data)) {
                // ONPS 队列有数据，或这个 socket 只绑定了 ONPS，就从真实网络路径收。
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
                    // ONPS 返回的 from_port 是主机字节序；返回给用户要转成网络字节序。
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
                // loopback UDP 没有 datagram 时按阻塞/非阻塞/超时语义等待。
                if (cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending(cur)) {
                    _lock.release();
                    return -EINTR;
                }
                if (is_nonblocking_request(flags)) {
                    // 非阻塞 UDP recvfrom 没数据时返回 EAGAIN。
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
                // 读半边关闭且没有数据，返回 0。
                _lock.release();
                return 0;
            }

            // UDP 保留报文边界：一次 recvfrom 只取队首这一枚 datagram。
            loopback_datagram &packet = _datagram_queue.front();
            size_t copy_len = eastl::min(len, packet.data.size());
            memcpy(data, packet.data.data(), copy_len);

            if (src_addr && addrlen && *addrlen > 0) {
                // 用户请求源地址时，把发送方地址复制给调用者。
                socklen_t copy_addr_len = eastl::min(*addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                memcpy(src_addr, &packet.src_addr, copy_addr_len);
                *addrlen = sizeof(struct sockaddr_in);
            }

            if (!(flags & MSG_PEEK)) {
                // MSG_PEEK 不消费 datagram；普通 recvfrom 会弹出队首报文。
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
            // RAW ICMP 接收路径：从 ONPS ICMP input 中取 ICMP 包，再补一个 IPv4 头给用户。
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
            // Linux raw socket 接收 ICMP 时通常能看到 IP 头，这里手工构造一个最小 IPv4 头。
            build_raw_ipv4_header(ip_header,
                                  from_ip,
                                  local_ip,
                                  ttl,
                                  static_cast<uint16_t>(received));

            size_t total_len = sizeof(ip_header) + static_cast<size_t>(received);
            size_t copy_len = eastl::min(len, total_len);
            size_t header_copy = eastl::min(copy_len, sizeof(ip_header));
            // 先复制构造出的 IPv4 头，再复制 ONPS 给出的 ICMP 数据。
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
            // TCP recvfrom 基本等同 recv；如果用户传了 src_addr，则返回已连接 peer 地址。
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
        // shutdown 只关闭读/写方向，不负责释放 fd 引用。
        _lock.acquire();
        
        if (_state != SocketState::CONNECTED) {
            _lock.release();
            return -ENOTCONN;
        }

        SOCKET onps_close_socket = INVALID_SOCKET;
        // how 决定关闭哪个方向：0 关读、1 关写、2 同时关读写。
        // 这里只修改 socket 的半关闭状态，不删除 fd，也不释放 socket_file。
        switch (how) {
            case 0: // SHUT_RD
                // 关闭读半边：本端后续 recv 直接 EOF，并丢弃已经排队的接收数据。
                _read_shutdown = true;
                _recv_buffer.clear();
                _datagram_queue.clear();
                break;
            case 1: // SHUT_WR
                // 关闭写半边：本端不能再 send，并清掉 MSG_MORE 暂存。
                _write_shutdown = true;
                _send_buffer.clear();
                _pending_send_has_addr = false;
                break;
            case 2: // SHUT_RDWR
                // 双向关闭：本端状态直接进入 CLOSED；如果有 ONPS 句柄，稍后在锁外关闭。
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
            // 写半边关闭要通知对端：对端读完已有数据后应看到 EOF。
            peer = _peer;
        }

        // 唤醒本端可能阻塞的 recv/recvfrom。
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
        _lock.release();

        if (onps_close_socket != INVALID_SOCKET) {
            // 关闭 ONPS 句柄可能进入协议栈，放在 socket_file 锁外避免锁嵌套。
            close_onps_handle(_type, onps_close_socket);
        }

        if (peer != nullptr) {
            // 通知 loopback/AF_UNIX 对端“我的写半边已经关闭”。
            peer->_lock.acquire();
            peer->mark_stream_peer_write_shutdown_locked();
            peer->_lock.release();
        }
        return 0;
    }

    int socket_file::setsockopt(int level, int optname, const void *optval, socklen_t optlen)
    {
        // setsockopt 接收的是 syscall 层已经 copy_in 到内核的 optval。
        if (!optval) {
            return -EFAULT;
        }
        if (optlen == 0) {
            return -EINVAL;
        }

        _lock.acquire();
        
        if (level == SOL_SOCKET) {
            if (is_receive_timeout_option(optname) || is_send_timeout_option(optname)) {
                // SO_RCVTIMEO/SO_SNDTIMEO 使用 timeval 结构。
                if (optlen < sizeof(socket_timeval)) {
                    _lock.release();
                    return -EINVAL;
                }

                socket_timeval timeout{};
                memcpy(&timeout, optval, sizeof(timeout));
                uint64 timeout_us = 0;
                // timeval 合法性检查集中在 socket_timeout_to_usec。
                if (!socket_timeout_to_usec(timeout.tv_sec, timeout.tv_usec, timeout_us)) {
                    _lock.release();
                    return -EDOM;
                }

                // 64 位 Linux 上 libc 可能使用 OLD 编号 20/21；
                // 同时接受 NEW 编号，保持 time64 ABI 选项兼容。
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
                    // 当前端口表没有完整复用语义，但保留标志供兼容和后续扩展。
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
                    // 避免用户态网络程序因非核心选项失败而退出。
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
            if (optname == k_ip_tos) {
                // OpenSSH 会在连接前后设置 IP_TOS/IPQoS。F7LY 暂不把 TOS 写入真实 IPv4 包头，
                // 但接受并保存这个 8 bit 值，避免兼容程序因非关键 QoS 选项失败而报警。
                if (optlen < sizeof(int)) {
                    _lock.release();
                    return -EINVAL;
                }
                int tos = *static_cast<const int*>(optval);
                if (tos < 0 || tos > 0xff) {
                    _lock.release();
                    return -EINVAL;
                }
                _ip_tos = tos;
                _lock.release();
                return 0;
            }
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
        // getsockopt 的 optval/optlen 已由 syscall 层准备成内核缓冲和内核变量。
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
                // 返回当前 SO_RCVTIMEO。
                int result = copy_socket_timeval_option(optval, optlen,
                                                        _recv_timeout_sec,
                                                        _recv_timeout_usec);
                _lock.release();
                return result;
            }

            if (is_send_timeout_option(optname)) {
                // 返回当前 SO_SNDTIMEO。
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
                    // 非阻塞 connect 的完成状态通过 SO_ERROR 暴露给用户态。
                    if (_onps_active && _type == SocketType::TCP && _onps_socket != INVALID_SOCKET)
                    {
                        int connect_error = onps_tcp_connect_so_error(_onps_socket);
                        if (connect_error == 0 && _state == SocketState::CONNECTING)
                        {
                            _state = SocketState::CONNECTED;
                        }
                        *static_cast<int*>(optval) = connect_error;
                    }
                    else
                    {
                        *static_cast<int*>(optval) = 0;
                    }
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_ACCEPTCONN:
                    // 是否已经 listen。
                    *static_cast<int*>(optval) = _state == SocketState::LISTENING ? 1 : 0;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_SNDBUF:
                case SO_RCVBUF:
                    // 返回一个稳定的默认缓冲大小，兼容用户态探测。
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
                    // 返回 socket 创建时记录的 protocol 参数。
                    *static_cast<int*>(optval) = _protocol;
                    *optlen = sizeof(int);
                    _lock.release();
                    return 0;

                case SO_DOMAIN:
                    // 返回 AF_INET/AF_UNIX/AF_INET6。
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
                // 用户态有时会读取拥塞控制算法名；loopback 没有真实拥塞控制，返回常见值 cubic。
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
            if (optname == k_ip_tos) {
                int result = copy_socket_int_option(optval, optlen, _ip_tos);
                _lock.release();
                return result;
            }
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
        // getsockname 返回本端地址。这里收到的是用户虚拟地址指针，需要自己 copy_in/copy_out。
        if (!addr || !addrlen) {
            return -EFAULT;
        }
        if ((uint64)addr < sizeof(struct sockaddr) || (uint64)addrlen < sizeof(socklen_t)) {
            return -EFAULT;
        }

        _lock.acquire();
        if (_family == SocketFamily::UNIX) {
            // AF_UNIX 返回 sockaddr_un。
            proc::Pcb *p = proc::k_pm.get_cur_pcb();
            mem::PageTable *pt = p->get_pagetable();
            socklen_t requested_len = 0;
            // addrlen 输入值表示用户 addr 缓冲区大小。
            if (mem::k_vmm.copy_in(*pt, &requested_len, (uint64)addrlen, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            if (requested_len > k_max_user_sockaddr_len) {
                _lock.release();
                return -EINVAL;
            }
            socklen_t copy_len = eastl::min(requested_len, static_cast<socklen_t>(sizeof(struct sockaddr_un)));
            // 只按用户缓冲区大小复制，避免越界写用户空间。
            if (mem::k_vmm.copy_out(*pt, (uint64)addr, &_local_unix_addr, copy_len) < 0) {
                _lock.release();
                return -EFAULT;
            }
            socklen_t actual_len = sizeof(struct sockaddr_un);
            // 回写真实地址长度。
            if (mem::k_vmm.copy_out(*pt, (uint64)addrlen, &actual_len, sizeof(socklen_t)) < 0) {
                _lock.release();
                return -EFAULT;
            }
            _lock.release();
            return 0;
        }
        if (_onps_socket != INVALID_SOCKET) {
            // ONPS 可能自动选择本地地址/端口，查询前同步一次。
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
        // getpeername 返回对端地址，只有 CONNECTED socket 有对端。
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
            // AF_UNIX 返回对端路径地址。
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
        // 非阻塞有两种来源：fd 本身 O_NONBLOCK，或单次调用带 MSG_DONTWAIT。
        return !_blocking || (flags & MSG_DONTWAIT);
    }

    bool socket_file::onps_udp_read_ready_locked() const
    {
        // 判断真实网络 UDP 队列是否有数据；调用者必须已经持有 _lock。
        return _type == SocketType::UDP &&
               _onps_bound &&
               _onps_socket != INVALID_SOCKET &&
               onps_input_has_pending_data(static_cast<INT>(_onps_socket));
    }

    bool socket_file::udp_read_ready_locked() const
    {
        // INADDR_ANY UDP socket 会同时注册 loopback 和 ONPS。
        // read_ready 必须同时检查两条接收队列，避免 DNS 等外网回包
        // 已进入 ONPS input 后仍被 poll 判定为不可读。
        return onps_udp_read_ready_locked() || !_datagram_queue.empty();
    }

    int socket_file::ensure_onps_socket_locked()
    {
        // 懒创建 ONPS TCP/UDP socket。调用者必须持有 _lock。
        if (_onps_socket != INVALID_SOCKET)
        {
            return 0;
        }
        if (!can_use_onps_socket(_family, _type))
        {
            return -ENETUNREACH;
        }

        EN_ONPSERR error = ERRNO;
        // ONPS socket API 类似 BSD socket，但返回 ONPS 自己的句柄和错误码。
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
        // RAW ICMP 不走 ONPS BSD socket，而是直接申请一个 ICMP input。
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
        // RAW ICMP input 创建后即可视为已绑定。
        if (_state == SocketState::CREATED)
        {
            _state = SocketState::BOUND;
        }
        return 0;
    }

    int socket_file::bind_onps_locked(const struct sockaddr_in &addr)
    {
        // 把 socket_file 的本地地址同步绑定到 ONPS 协议栈。
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
            // ONPS bind 接口接受字符串 IP；0.0.0.0 用 nullptr 表示任意地址。
            ipv4_to_string(addr.sin_addr, ip);
            ip_arg = ip;
        }

        USHORT host_port = socket_port_to_host(addr.sin_port);
        // sockaddr_in.sin_port 是网络字节序，ONPS bind 需要主机字节序端口。
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
        // 把 F7LY socket 的阻塞/超时设置同步到 ONPS socket。
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
        // MSG_MORE 使用的暂存逻辑。调用者必须持有 _lock。
        if (len > 0 && data == nullptr) {
            return -EFAULT;
        }
        if (_type == SocketType::UDP && dest_addr != nullptr) {
            // UDP 暂存期间必须固定目标地址，否则无法定义最终 datagram 发往哪里。
            if (!_pending_send_has_addr) {
                _pending_send_addr = *dest_addr;
                _pending_send_has_addr = true;
            } else if (!pending_send_destination_matches_locked(dest_addr)) {
                return -EINVAL;
            }
        }
        if (_send_buffer.size() + len > static_cast<size_t>(k_default_socket_buffer_size)) {
            // 防止用户用 MSG_MORE 无限堆积内核内存。
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
        // 自动分配 loopback 本地端口，常用于未 bind 的 connect/listen/sendto。
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
        // 从临时端口范围中找一个没有被同类型 socket 占用的端口。
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
        // 自动 bind 的本地地址使用 127.0.0.1。
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
        // loopback TCP/AF_UNIX 的核心发送函数：把 data 追加到 peer 的接收缓冲。
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
            // 只持有对端锁，避免双向 send 时两个 socket 互相拿锁死锁。
            peer->_lock.acquire();
            while (peer->_recv_buffer.size() >= k_tcp_recv_buffer_max_bytes &&
                   peer->stream_receive_open_locked())
            {
                if (nonblocking)
                {
                    peer->_lock.release();
                    return queued > 0 ? static_cast<int>(queued) : -EAGAIN;
                }
                proc::k_pm.sleep(&peer->_recv_buffer, &peer->_lock);
            }

            if (!peer->stream_receive_open_locked())
            {
                // 对端读半边关闭或 socket 关闭，写入应得到 EPIPE。
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
            // 追加到对端 stream 缓冲；TCP 不保留单次 send 的边界。
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
        if (!stream_write_open_locked())
        {
            return -EPIPE;
        }

        return enqueue_stream_data_to_peer(_peer, data, len, !_blocking);
    }

    int socket_file::enqueue_datagram(const struct sockaddr_in *src_addr, const uint8_t *data, size_t len)
    {
        // loopback UDP 的核心入队函数：一次调用入队一枚完整 datagram。
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
            // 记录源地址，recvfrom 时返回给用户。
            packet.src_addr = *src_addr;
        }
        packet.data.resize(len);
        if (len > 0)
        {
            memcpy(packet.data.data(), data, len);
        }
        _datagram_queue_bytes += len;
        _datagram_queue.push_back(packet);
        // 唤醒阻塞在 recvfrom 的读端。
        proc::k_pm.wakeup(&_datagram_queue);
        return static_cast<int>(len);
    }

    void socket_file::attach_loopback_peer(socket_file *peer)
    {
        // socketpair 或 loopback connect 成功后，通过这个函数建立 peer 关系。
        _lock.acquire();
        _peer = peer;
        _peer_closed = peer == nullptr;
        _peer_write_shutdown = peer == nullptr;
        _state = peer == nullptr ? SocketState::CLOSED : SocketState::CONNECTED;
        _lock.release();
    }

    bool socket_file::stream_read_eof_locked() const
    {
        // 本端关闭读半边、对端关闭写半边或对端彻底关闭，都会让本地 stream 读到 EOF。
        return _read_shutdown || _peer_write_shutdown || _peer_closed;
    }

    bool socket_file::stream_read_ready_locked() const
    {
        // 有数据或者已经 EOF，都认为读就绪；EOF 时 recv 会立刻返回 0。
        return !_recv_buffer.empty() || stream_read_eof_locked();
    }

    bool socket_file::stream_write_open_locked() const
    {
        // 写半边未关闭、对端未关闭且 peer 存在，才允许继续写。
        return !_write_shutdown && !_peer_closed && _peer != nullptr;
    }

    bool socket_file::stream_receive_open_locked() const
    {
        // 对端写入前检查本端是否还愿意接收数据。
        return !_read_shutdown && _state != SocketState::CLOSED;
    }

    void socket_file::mark_stream_peer_closed_locked(socket_file *peer)
    {
        if (_peer == peer)
        {
            _peer = nullptr;
        }
        _peer_closed = true;
        _peer_write_shutdown = true;
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
    }

    void socket_file::mark_stream_peer_write_shutdown_locked()
    {
        // shutdown(SHUT_WR) 是半关闭：只表示对端不会再写，不能阻止本端继续写回。
        _peer_write_shutdown = true;
        proc::k_pm.wakeup(&_recv_buffer);
        proc::k_pm.wakeup(&_datagram_queue);
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

#pragma once
#include "fs/vfs/file/file.hh"
#include "fs/vfs/file/socket_defs.hh"
#include "printer.hh"
#include "sys/syscall_defs.hh"
// #include "net/net.hh"
#include "devs/spinlock.hh"

// 前向声明，避免包含整个onps头文件
typedef int SOCKET;
#define INVALID_SOCKET -1

namespace fs
{
    // Socket状态
    enum class SocketState
    {
        CREATED,    // 已创建
        BOUND,      // 已绑定
        LISTENING,  // 监听中
        CONNECTED,  // 已连接
        CLOSED      // 已关闭
    };

    // Socket类型
    enum class SocketType
    {
        TCP = SOCK_STREAM,
        UDP = SOCK_DGRAM,
        RAW = SOCK_RAW
    };

    // Socket协议族
    enum class SocketFamily
    {
        INET = AF_INET,
        INET6 = AF_INET6,
        UNIX = AF_UNIX
    };

    class socket_file : public file
    {
    private:
        SocketState _state;
        SocketType _type;
        SocketFamily _family;
        int _protocol;
        
        // onps socket句柄
        SOCKET _onps_socket;
        
        // 地址信息
        struct sockaddr_in _local_addr;
        struct sockaddr_in _remote_addr;
        
        // TCP连接队列
        eastl::vector<socket_file*> _pending_connections;
        int _backlog;
        
        // 数据缓冲区
        eastl::vector<uint8_t> _recv_buffer;
        eastl::vector<uint8_t> _send_buffer;
        
        // 标志位
        bool _blocking;
        bool _reuse_addr;
        
        SpinLock _lock;

    public:
        socket_file(int domain, int type, int protocol);
        socket_file(FileAttrs attrs, int domain, int type, int protocol);
        virtual ~socket_file();

        // 设置onps socket句柄
        void set_onps_socket(SOCKET onps_socket) { _onps_socket = onps_socket; }
        SOCKET get_onps_socket() const { return _onps_socket; }

        // 继承自file类的虚函数
        virtual long read(uint64 buf, size_t len, long off, bool upgrade) override;
        virtual long write(uint64 buf, size_t len, long off, bool upgrade) override;
        virtual bool read_ready() override;
        virtual bool write_ready() override;
        virtual off_t lseek(off_t offset, int whence) override;
        virtual size_t read_sub_dir(ubuf &dst) override;

        // Socket特有的操作
        int bind(const struct sockaddr *addr, socklen_t addrlen);
        int listen(int backlog);
        socket_file* accept(struct sockaddr *addr, socklen_t *addrlen);
        int connect(const struct sockaddr *addr, socklen_t addrlen);
        int send(const void *buf, size_t len, int flags);
        int recv(void *buf, size_t len, int flags);
        int sendto(const void *buf, size_t len, int flags, 
                  const struct sockaddr *dest_addr, socklen_t addrlen);
        int recvfrom(void *buf, size_t len, int flags,
                    struct sockaddr *src_addr, socklen_t *addrlen);
        int shutdown(int how);
        int setsockopt(int level, int optname, const void *optval, socklen_t optlen);
        int getsockopt(int level, int optname, void *optval, socklen_t *optlen);
        int getsockname(struct sockaddr *addr, socklen_t *addrlen);
        int getpeername(struct sockaddr *addr, socklen_t *addrlen);

        // 状态查询
        SocketState get_state() const { return _state; }
        SocketType get_type() const { return _type; }
        SocketFamily get_family() const { return _family; }
        bool is_blocking() const { return _blocking; }

        // 内部辅助函数
    private:
        bool is_valid_address(const struct sockaddr *addr, socklen_t addrlen);
        int copy_sockaddr_to_user(struct sockaddr *user_addr, socklen_t *user_addrlen,
                                 const struct sockaddr_in *kernel_addr);
        int copy_sockaddr_from_user(struct sockaddr_in *kernel_addr,
                                   const struct sockaddr *user_addr, socklen_t addrlen);
        void set_error(int error_code);
        bool can_accept_connection();
        void add_to_pending_queue(socket_file* client_socket);
        socket_file* get_from_pending_queue();
    };
}

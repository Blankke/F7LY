#include "types.hh"
#include "printer.hh"

namespace net
{
    typedef unsigned short sa_family_t;
    typedef uint32 socklen_t; // socklen_t is used to specify the size of the address structure
    struct sockaddr
    {
        sa_family_t sin_family; // 地址族（Address Family），也就是地址类型
        char sa_data[14];       // IP地址和端口号
    };
    int socket(int domain, int type, int protocol);
    int bind(int sockfd, const sockaddr *addr, socklen_t addrlen);
    int listen(int sockfd, int backlog);
    int connect(int sockfd, const sockaddr *addr, socklen_t addrlen);
    int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    int setsockopt(int socket, int level, int option_name, const void *option_value, size_t option_len);
    int sendto(int s, const void *msg, int len, unsigned int flags, const struct sockaddr *to, int tolen);
    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);
    int accept(int sock, struct sockaddr *addr, socklen_t *addrlen);
}
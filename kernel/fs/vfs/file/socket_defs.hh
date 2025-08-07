#pragma once

// Socket protocols families
#define AF_UNSPEC       0       /* unspecified */
#define AF_UNIX         1       /* local to host (pipes, portals) */
#define AF_LOCAL        AF_UNIX /* POSIX name for AF_UNIX */
#define AF_INET         2       /* internetwork: UDP, TCP, etc. */
#define AF_INET6        10      /* IPv6 */

// Protocol families (same as address families)
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_LOCAL
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

// Socket types
#define SOCK_STREAM     1       /* stream socket */
#define SOCK_DGRAM      2       /* datagram socket */
#define SOCK_RAW        3       /* raw-protocol interface */
#define SOCK_SEQPACKET  5       /* sequenced packet stream */

// Socket level
#define SOL_SOCKET      1       /* socket level */

// Socket options
#define SO_REUSEADDR    2       /* reuse address */
#define SO_REUSEPORT    15      /* reuse port */
#define SO_KEEPALIVE    9       /* keep connections alive */
#define SO_LINGER       13      /* linger on close if data present */

// Shutdown options
#define SHUT_RD         0       /* shut down the reading side */
#define SHUT_WR         1       /* shut down the writing side */
#define SHUT_RDWR       2       /* shut down both sides */

// Error codes
#define EAFNOSUPPORT    97      /* Address family not supported by protocol */
#define ENOTSOCK        88      /* Socket operation on non-socket */
#define EOPNOTSUPP      95      /* Operation not supported on transport endpoint */
#define EADDRINUSE      98      /* Address already in use */
#define EADDRNOTAVAIL   99      /* Cannot assign requested address */
#define ENETDOWN        100     /* Network is down */
#define ENETUNREACH     101     /* Network is unreachable */
#define ECONNABORTED    103     /* Software caused connection abort */
#define ECONNRESET      104     /* Connection reset by peer */
#define ENOBUFS         105     /* No buffer space available */
#define EISCONN         106     /* Transport endpoint is already connected */
#define ENOTCONN        107     /* Transport endpoint is not connected */
#define ETIMEDOUT       110     /* Connection timed out */
#define ECONNREFUSED    111     /* Connection refused */

// Socket address structure
struct sockaddr {
    unsigned short sa_family;    /* address family, AF_xxx */
    char sa_data[14];           /* 14 bytes of protocol address */
};

// Internet socket address structure
struct sockaddr_in {
    unsigned short sin_family;  /* address family: AF_INET */
    unsigned short sin_port;    /* port number */
    unsigned int sin_addr;      /* internet address */
    char sin_zero[8];          /* padding */
};

// Socket address length type
typedef unsigned int socklen_t;

// Internet address
#ifndef s_addr  
struct in_addr {
    unsigned int s_addr;
};
#define s_addr s_addr
#endif

// IPv6 address
struct in6_addr {
    unsigned char s6_addr[16];
};

// IPv6 socket address structure
struct sockaddr_in6 {
    unsigned short sin6_family;   /* AF_INET6 */
    unsigned short sin6_port;     /* port number */
    unsigned int sin6_flowinfo;   /* IPv6 flow information */
    struct in6_addr sin6_addr;    /* IPv6 address */
    unsigned int sin6_scope_id;   /* scope id (new in 2.4) */
};

// Unix domain socket address structure
struct sockaddr_un {
    unsigned short sun_family;   /* AF_UNIX */
    char sun_path[108];         /* pathname */
};

// Message header structure for sendmsg/recvmsg
struct msghdr {
    void *msg_name;             /* optional address */
    socklen_t msg_namelen;      /* size of address */
    struct iovec *msg_iov;      /* scatter/gather array */
    size_t msg_iovlen;          /* number of elements in msg_iov */
    void *msg_control;          /* ancillary data, see below */
    size_t msg_controllen;      /* ancillary data buffer len */
    int msg_flags;              /* flags on received message */
};

// sendmsg/recvmsg flags
#define MSG_OOB         0x01    /* process out-of-band data */
#define MSG_PEEK        0x02    /* peek at incoming message */
#define MSG_DONTROUTE   0x04    /* don't use local routing */
#define MSG_CTRUNC      0x08    /* control data truncated */
#define MSG_TRUNC       0x20    /* data truncated */
#define MSG_DONTWAIT    0x40    /* nonblocking request */
#define MSG_WAITALL     0x100   /* wait for full request or error */

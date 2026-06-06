#pragma once

#include "syscall_abi.hh"

namespace mem
{
    class PageTable;
}

namespace net
{
    namespace abi = syscall::abi;

    class SocketIoctlCompat
    {
    public:
        // 网络 ioctl 只暴露当前内核确实能模拟的 loopback 设备视图；
        // 后续接入真实网卡枚举时，应扩展这个类，而不是把 ifreq 逻辑塞回 handler。
        static constexpr u32 k_siocatmark = 0x8905;
        static constexpr u32 k_siocgifconf = 0x8912;
        static constexpr u32 k_siocgifflags = 0x8913;
        static constexpr u32 k_siocsifflags = 0x8914;

        static constexpr short k_iff_up = 0x1;
        static constexpr short k_iff_loopback = 0x8;
        static constexpr short k_iff_running = 0x40;
        static constexpr short k_loopback_flags = k_iff_up | k_iff_loopback | k_iff_running;

        static bool is_loopback_ifname(const char *name);
        static void fill_loopback_ifreq(abi::SocketIfreq &req);
    };

    class SocketMessageCompat
    {
    public:
        // recvmmsg 目前只需要验证 timeout ABI；真正的收包路径仍由 socket_file 承担。
        static int validate_recvmmsg_timeout(mem::PageTable *pt, uint64 timeout_addr);
    };
}

#include "socket_compat.hh"

#include "klib.hh"
#include "syscall_defs.hh"
#include "timer_manager.hh"
#include "virtual_memory_manager.hh"

namespace net
{
    bool SocketIoctlCompat::is_loopback_ifname(const char *name)
    {
        return name[0] == 'l' && name[1] == 'o' && name[2] == '\0';
    }

    void SocketIoctlCompat::fill_loopback_ifreq(abi::SocketIfreq &req)
    {
        memset(&req, 0, sizeof(req));
        req.ifr_name[0] = 'l';
        req.ifr_name[1] = 'o';

        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr = 0x0100007f;
        memcpy(&req.ifr_addr, &addr, sizeof(addr));
    }

    int SocketMessageCompat::validate_recvmmsg_timeout(mem::PageTable *pt,
                                                       uint64 timeout_addr)
    {
        if (timeout_addr == 0)
        {
            return 0;
        }

        tmm::timespec timeout{};
        if (mem::k_vmm.copy_in(*pt, &timeout, timeout_addr, sizeof(timeout)) < 0)
        {
            return syscall::SYS_EFAULT;
        }
        if (timeout.tv_sec < 0 ||
            timeout.tv_nsec < 0 ||
            timeout.tv_nsec >= abi::k_nsec_per_sec)
        {
            return syscall::SYS_EINVAL;
        }
        return 0;
    }
}

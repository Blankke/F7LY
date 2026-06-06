#pragma once

#include "types.hh"
#include "fs/vfs/file/socket_defs.hh"

namespace syscall::abi
{
    // 这里集中保存“用户态可见的 Linux ABI 布局”和常量。
    // syscall_handler 只负责 copy_in/copy_out，不能在分发文件里重新定义这些结构。
    constexpr long k_nsec_per_sec = 1000000000L;
    constexpr int k_eidrm = 43;
    constexpr int k_xattr_create = 0x1;
    constexpr int k_xattr_replace = 0x2;

    constexpr uint k_mmsg_max_vlen = 1024;

    constexpr int k_epoll_ctl_add = 1;
    constexpr int k_epoll_ctl_del = 2;
    constexpr int k_epoll_ctl_mod = 3;
    constexpr uint32 k_epollin = 0x001u;
    constexpr uint32 k_epollpri = 0x002u;
    constexpr uint32 k_epollout = 0x004u;
    constexpr uint32 k_epollerr = 0x008u;
    constexpr uint32 k_epollhup = 0x010u;
    constexpr uint32 k_epollrdhup = 0x2000u;
    constexpr uint32 k_epolloneshot = 0x40000000u;
    constexpr uint32 k_epollet = 0x80000000u;
    constexpr uint32 k_epoll_interest_mask =
        k_epollin | k_epollpri | k_epollout | k_epollerr | k_epollhup | k_epollrdhup;

    constexpr int k_f_owner_tid = 0;
    constexpr int k_f_owner_pid = 1;
    constexpr int k_f_owner_pgrp = 2;

    constexpr int k_ifnamsiz = 16;

    struct SocketIfreq
    {
        char ifr_name[k_ifnamsiz];
        union
        {
            ::sockaddr ifr_addr;
            short ifr_flags;
            char ifr_padding[24];
        };
    };

    struct SocketIfconf
    {
        int ifc_len;
        uint64 ifc_buf;
    };

    struct KernelEpollEvent
    {
        uint32 events;
        // 64 位 Linux 用户态的 struct epoll_event 在 events 和 data 之间保留
        // 4 字节对齐空洞。内核 copy_in/copy_out 必须使用 16 字节布局，否则一次
        // 返回多个事件时后一项会错位。
        uint32 pad = 0;
        uint64 data = 0;
    };
    static_assert(sizeof(KernelEpollEvent) == 16,
                  "epoll event ABI must match 64-bit user layout");

    struct KernelTermios
    {
        uint32 c_iflag;
        uint32 c_oflag;
        uint32 c_cflag;
        uint32 c_lflag;
        unsigned char c_line;
        unsigned char c_cc[19];
    };
    static_assert(sizeof(KernelTermios) == 36,
                  "TCGETS must use Linux kernel termios ABI");

    struct KernelTermio
    {
        uint16 c_iflag;
        uint16 c_oflag;
        uint16 c_cflag;
        uint16 c_lflag;
        unsigned char c_line;
        unsigned char c_cc[8];
    };

    struct KernelTimeValOld
    {
        long tv_sec;
        long tv_usec;
    };

    struct KernelITimerValOld
    {
        KernelTimeValOld it_interval;
        KernelTimeValOld it_value;
    };

    struct KernelFOwnerEx
    {
        int type;
        int pid;
    };

    struct UserTimespec64
    {
        long tv_sec;
        long tv_nsec;
    };

    union KernelSigvalCompat
    {
        int sival_int;
        uint64_t sival_ptr;
    };

    struct KernelSigeventCompat
    {
        KernelSigvalCompat sigev_value;
        int sigev_signo;
        int sigev_notify;
        union
        {
            char __pad[64 - sizeof(KernelSigvalCompat) - 2 * sizeof(int)];
            int sigev_notify_thread_id;
            struct
            {
                uint64_t sigev_notify_function;
                uint64_t sigev_notify_attributes;
            } __sev_thread;
        } __sev_fields;
    };
    static_assert(sizeof(KernelSigeventCompat) == 64,
                  "Linux 64-bit sigevent ABI size mismatch");

    struct KernelTimexOld
    {
        unsigned int modes;
        int _pad0;
        long offset;
        long freq;
        long maxerror;
        long esterror;
        int status;
        int _pad1;
        long constant;
        long precision;
        long tolerance;
        KernelTimeValOld time;
        long tick;
        long ppsfreq;
        long jitter;
        int shift;
        int _pad2;
        long stabil;
        long jitcnt;
        long calcnt;
        long errcnt;
        long stbcnt;
        int tai;
        int reserved[11];
    };
    static_assert(sizeof(KernelTimexOld) == 208,
                  "Linux timex ABI size mismatch");
}

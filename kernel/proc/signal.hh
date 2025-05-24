#pragma once

#include "types.hh"

namespace proc
{
    namespace ipc
    {
        namespace signal
        {

            constexpr int SIGKILL = 9;
            constexpr int SIGSTOP = 19;
            constexpr int SIGQUIT = 3;
            constexpr int SIGRTMAX = 64;

            constexpr int SIG_BLOCK = 0;
            constexpr int SIG_UNBLOCK = 1;
            constexpr int SIG_SETMASK = 2;

            // 简化版 sigset_t，实际你可以用 bitset 或其他方式扩展
            typedef struct
            {
                uint64_t sig[1]; // 最多 64 个信号
            } sigset_t;

            // 简化版 sigaction
            typedef struct sigaction
            {
                void (*sa_handler)(int); // 信号处理函数
                sigset_t sa_mask;        // 处理期间阻塞的信号
                int sa_flags;            // 行为标志（未用）
            } sigaction;

        } // namespace signal
    } // namespace ipc
} // namespace proc

#pragma once

#include "types.hh"
#include "trapframe.hh"

namespace proc
{
    class Pcb;
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
            constexpr int SIGCHLD = 17;
            enum class SigActionFlags : uint64_t
            {
                NONE = 0,
                NOCLDSTOP = 1 << 0,
                NOCLDWAIT = 1 << 1,
                SIGINFO = 1 << 2,
                ONSTACK = 0x08000000,
                RESTART = 0x10000000,
                NODEFER = 0x40000000,
                RESETHAND = 0x80000000,
                INTERRUPT = 0x20000000,
                RESTORER = 0x04000000,
            };
            // 简化版 sigset_t，实际你可以用 bitset 或其他方式扩展
            typedef struct
            {
                uint64 sig[1]; // 最多 64 个信号
            } sigset_t;

            struct signal_frame
            {
                sigset_t mask;
                TrapFrame tf;
                signal_frame *next;
            };

            struct signalstack
            {
                uint64 sp;
                uint32 flags;
                uint32 pad;
                uint64 size;
            };

            struct machinecontext
            {
                TrapFrame gp; // gerneral purpose registers
                // todo : 没有浮点型
            };

            // ustack
            struct ustack
            {
                uint64 flags;            // 0x00 标志位，用于描述信号处理相关的状态。
                uint64 link;             // 0x08 链接到下一个信号栈的指针（如有嵌套信号处理）。
                signalstack stack;       // 0x10 信号处理时使用的备用栈信息结构体。
                uint64 sigmask;          // 0x30 信号屏蔽字，指示当前被屏蔽的信号集合。
                uint64 __pad[16];        // 0x38 填充字段，用于结构体对齐或将来扩展。
                machinecontext mcontext; // 0xB8 保存信号发生时的机器上下文（寄存器等）
            };

            // LinuxSigInfo
            struct LinuxSigInfo
            {
                uint32 si_signo;   // 0x00
                uint32 si_errno;   // 0x04
                uint32 si_code;    // 0x08
                uint32 _pad[29];   // 0x0C
                uint64 _align;     // 0x80
            };

            // 简化版 sigaction
            typedef struct sigaction
            {
                void (*sa_handler)(int); // 信号处理函数
                uint64 sa_flags;         // 行为标志
                uint64 sa_restorer;      // 恢复函数
                sigset_t sa_mask;        // 处理期间阻塞的信号
            } sigaction;
            int sigAction(int flag, sigaction *newact, sigaction *oldact);
            int sigprocmask(int how, sigset_t *newset, sigset_t *oldset, size_t sigsize);
            void handle_signal();
            void default_handle(Pcb *p, int signum);
            void add_signal(proc::Pcb *p, int sig);
            void do_handle(proc::Pcb *p, int signum, sigaction *act);
            void sig_return();

            // tool
            bool is_valid(int sig);
            bool sig_is_member(const uint64 set, int n_sig);
            bool is_ignored(Pcb *now_p, int sig);
            void clear_signal(Pcb *now_p, int sig);
        } // namespace signal
    } // namespace ipc
} // namespace proc

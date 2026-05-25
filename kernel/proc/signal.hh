#pragma once

#include "types.hh"
#include "trapframe.hh"
#include <stddef.h>

namespace proc
{
    class Pcb;
    namespace ipc
    {
        namespace signal
        {

            // Standard signal numbers (following Linux convention)
            constexpr int SIGHUP = 1;
            constexpr int SIGINT = 2;
            constexpr int SIGQUIT = 3;
            constexpr int SIGILL = 4;
            constexpr int SIGTRAP = 5;
            constexpr int SIGABRT = 6;
            constexpr int SIGBUS = 7;
            constexpr int SIGFPE = 8;
            constexpr int SIGKILL = 9;
            constexpr int SIGUSR1 = 10;
            constexpr int SIGSEGV = 11;
            constexpr int SIGUSR2 = 12;
            constexpr int SIGPIPE = 13;
            constexpr int SIGALRM = 14;
            constexpr int SIGTERM = 15;
            constexpr int SIGSTKFLT = 16;
            constexpr int SIGCHLD = 17;
            constexpr int SIGCONT = 18;
            constexpr int SIGSTOP = 19;
            constexpr int SIGTSTP = 20;
            constexpr int SIGTTIN = 21;
            constexpr int SIGTTOU = 22;
            constexpr int SIGURG = 23;
            constexpr int SIGXCPU = 24;
            constexpr int SIGXFSZ = 25;
            constexpr int SIGVTALRM = 26;
            constexpr int SIGPROF = 27;
            constexpr int SIGWINCH = 28;
            constexpr int SIGPOLL = 29;  // Also known as SIGIO
            constexpr int SIGPWR = 30;
            constexpr int SIGSYS = 31;   // Also known as SIGUNUSED
            constexpr int SIGRTMAX = 64;
            
            constexpr int SIG_BLOCK = 0;
            constexpr int SIG_UNBLOCK = 1;
            constexpr int SIG_SETMASK = 2;
            
            // Signal stack flags
            constexpr int SS_ONSTACK = 1;
            constexpr int SS_DISABLE = 2;
            constexpr int SS_AUTODISARM = 4;
            
            // Signal stack size constants
#ifdef LOONGARCH
            // LoongArch 的 musl/glibc 用户 ABI 约定更大的最小信号栈。
            constexpr size_t MINSIGSTKSZ = 4096;
            constexpr size_t SIGSTKSZ = 16384;
#else
            constexpr size_t MINSIGSTKSZ = 2048;
            constexpr size_t SIGSTKSZ = 8192;
#endif
            
            // Signal handler type
            typedef void (*__sighandler_t)(int);
            
            // Special signal handler values
            #define	SIG_ERR	 ((__sighandler_t) -1)	/* Error return.  */
            #define	SIG_DFL	 ((__sighandler_t)  0)	/* Default action.  */
            #define	SIG_IGN	 ((__sighandler_t)  1)	/* Ignore signal.  */
            
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
                void *ss_sp;     // Base address of stack
                int ss_flags;    // Flags (SS_ONSTACK, SS_DISABLE, SS_AUTODISARM)
                size_t ss_size;  // Number of bytes in stack
            };

            // rt_sigaction(2) 进入内核时看到的不是 libc 暴露给应用的 struct sigaction，
            // 而是 libc 按 Linux syscall ABI 重新打包后的 k_sigaction。
            // 对于当前支持的 asm-generic 架构（RISC-V/LoongArch），布局固定为：
            //   handler -> flags -> restorer -> mask
            // 其中 mask 只占内核 sigset_t 的 8 字节，而不是 libc 用户 ABI 的 128 字节。
            struct kernel_sigaction_abi
            {
                union
                {
                    __sighandler_t sa_handler;
                    void (*sa_sigaction)(int, void *, void *);
                };
                uint64 sa_flags;
                void (*sa_restorer)(void);
                sigset_t sa_mask;
            };

#ifdef LOONGARCH
            constexpr size_t k_user_sigset_words = 16; // musl/glibc loongarch64: 128-byte sigset_t

            struct user_sigset
            {
                uint64 sig[k_user_sigset_words];
            };

            struct machinecontext
            {
                uint64 pc;         // 对应用户态 ucontext_t.uc_mcontext.__pc / MC_PC
                uint64 gregs[32];  // 对应 __gregs[32]
                uint32 flags;      // 对应 __flags
                // Linux LoongArch ABI 在这里不是额外的 pad 字段，而是一个按 16 字节对齐
                // 的可扩展上下文尾部。当前我们不保存 FPU/LSX/LASX 扩展上下文，但布局
                // 仍要和用户态头文件保持完全一致，否则 pthread/signal 相关测例读取
                // ucontext_t 时会把 mcontext 解析错位。
                uint64 extcontext[0] __attribute__((aligned(16)));
            };

            struct usercontext
            {
                uint64 flags;
                usercontext *link;
                signalstack stack;
                user_sigset sigmask;
                // musl 的 loongarch64 头文件显式保留了这个 pad 字段；
                // glibc 虽然字段名不同，但最终也会在 uc_mcontext 前留出同样的
                // 8 字节空洞，以满足 16 字节对齐要求。
                long uc_pad;
                machinecontext mcontext;
            };

            static_assert(sizeof(signalstack) == 24, "LoongArch signalstack ABI mismatch");
            static_assert(sizeof(user_sigset) == 128, "LoongArch sigset ABI mismatch");
            static_assert(sizeof(machinecontext) == 272, "LoongArch mcontext ABI mismatch");
            static_assert(offsetof(usercontext, mcontext) == 176, "LoongArch ucontext mcontext offset mismatch");
            static_assert(sizeof(usercontext) == 448, "LoongArch ucontext ABI mismatch");
#else
            struct generalregs{
                uint64 x[23]; // 通用寄存器 x0-x31
            };
            struct floatregs{
                uint64 f[23]; // 浮点寄存器 f0-f31
                uint32 fcsr; // 浮点控制和状态寄存器
            };

            struct machinecontext
            {
                generalregs gp; // gerneral purpose registers,  offset 0x00, size 0x100
                floatregs fp;   // floating point registers,    offset 0x100, size 0x108
            };
            // ustack
            struct usercontext
            {
                uint64 flags;                 // 0x00 标志位，用于描述信号处理相关的状态。
                usercontext *link;            // 0x08 链接到下一个信号栈的指针（如有嵌套信号处理）。
                signalstack stack;            // 0x10 信号处理时使用的备用栈信息结构体。
                machinecontext mcontext;      // 0x28 保存信号发生时的机器上下文（寄存器等），大小 0x208
                uint64 sigmask;               // 0x230 信号屏蔽字，指示当前被屏蔽的信号集合。
            };
#endif

            // LinuxSigInfo
            struct LinuxSigInfo
            {
                int32 si_signo;
                int32 si_errno;
                int32 si_code;
                uint8 _pad[128 - 3 * sizeof(int32)];
            };

            // 简化版 sigaction
            typedef struct sigaction
            {
                __sighandler_t sa_handler; // 信号处理函数
                uint64 sa_flags;          // 行为标志（匹配 rt_sigaction 原始 ABI）
                sigset_t sa_mask;         // 处理期间阻塞的信号
            } sigaction;
            
            // 信号默认行为结构体
            struct SignalAction {
                bool terminate;
                bool coredump;
            };
            
            int sigAction(int flag, sigaction *newact, sigaction *oldact);
            int sigprocmask(int how, sigset_t *newset, sigset_t *oldset, size_t sigsize);
            int sigsuspend(const sigset_t *mask);
            int sigaltstack(const signalstack *ss, signalstack *old_ss);
            void handle_signal();
            void handle_sync_signal();
            void default_handle(Pcb *p, int signum);
            SignalAction get_default_signal_action(int signum);
            void add_signal(proc::Pcb *p, int sig);
            void do_handle(proc::Pcb *p, int signum, sigaction *act);
            void sig_return();
            bool has_fatal_signal_pending(Pcb *p);
            bool has_unmasked_signal_pending(Pcb *p);

            // tool
            bool is_valid(int sig);
            bool is_sync_signal(int sig);
            bool sig_is_member(const uint64 set, int n_sig);
            bool is_ignored(Pcb *now_p, int sig);
            void clear_signal(Pcb *now_p, int sig);

            const uint64 guard = 0x11451416;
        } // namespace signal
    } // namespace ipc
} // namespace proc

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
            constexpr size_t MINSIGSTKSZ = 2048;
            constexpr size_t SIGSTKSZ = 8192;
            
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
                uint64 flags;            // 0x00 标志位，用于描述信号处理相关的状态。
                usercontext* link;            // 0x08 链接到下一个信号栈的指针（如有嵌套信号处理）。
                signalstack stack;       // 0x10 信号处理时使用的备用栈信息结构体。
                machinecontext mcontext; // 0x28 保存信号发生时的机器上下文（寄存器等），大小 0x208
                uint64 sigmask;          // 0x230 信号屏蔽字，指示当前被屏蔽的信号集合。
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
                __sighandler_t sa_handler; // 信号处理函数
                uint64 sa_flags;         // 行为标志
                uint64 sa_restorer;      // 恢复函数
                sigset_t sa_mask;        // 处理期间阻塞的信号
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

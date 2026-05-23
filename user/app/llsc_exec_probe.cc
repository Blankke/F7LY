#include "user.hh"

static volatile int g_probe = 0;
static volatile int g_sig_handled = 0;
static volatile int g_pc_rewrite_seen = 0;

extern "C" __attribute__((noreturn)) void after_pc_rewrite();

namespace
{
    constexpr long SYS_tkill = 130;
    constexpr long SYS_rt_sigaction = 134;
    constexpr long SYS_gettid = 178;
    constexpr int SIGUSR1 = 10;
    constexpr int SIGUSR2 = 12;
    constexpr unsigned long SA_SIGINFO = 0x00000004;
    constexpr unsigned long SA_RESTART = 0x10000000;

    struct kernel_sigset_t
    {
        unsigned long sig[1];
    };

    struct kernel_sigaction_abi
    {
        void (*handler)(int, void *, void *);
        unsigned long flags;
        void (*restorer)(void);
        kernel_sigset_t mask;
    };

    struct signal_stack_abi
    {
        void *ss_sp;
        int ss_flags;
        unsigned long ss_size;
    };

    struct machine_context_abi
    {
        unsigned long pc;
        unsigned long gregs[32];
        unsigned int flags;
        unsigned int padding;
    };

    struct user_context_abi
    {
        unsigned long flags;
        user_context_abi *link;
        signal_stack_abi stack;
        unsigned long sigmask[16];
        long uc_pad;
        machine_context_abi mcontext;
    };

    static long raw_syscall0(long nr)
    {
        register long a0 __asm__("$a0");
        register long a7 __asm__("$a7") = nr;
        __asm__ __volatile__("syscall 0" : "=r"(a0) : "r"(a7) : "memory");
        return a0;
    }

    static long raw_syscall2(long nr, long arg0, long arg1)
    {
        register long a0 __asm__("$a0") = arg0;
        register long a1 __asm__("$a1") = arg1;
        register long a7 __asm__("$a7") = nr;
        __asm__ __volatile__("syscall 0" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
        return a0;
    }

    static long raw_syscall4(long nr, long arg0, long arg1, long arg2, long arg3)
    {
        register long a0 __asm__("$a0") = arg0;
        register long a1 __asm__("$a1") = arg1;
        register long a2 __asm__("$a2") = arg2;
        register long a3 __asm__("$a3") = arg3;
        register long a7 __asm__("$a7") = nr;
        __asm__ __volatile__("syscall 0"
                             : "+r"(a0)
                             : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
                             : "memory");
        return a0;
    }

    void sigusr1_handler(int, void *, void *)
    {
        g_sig_handled = 1;
    }

    void sigusr2_rewrite_handler(int, void *, void *ctx)
    {
        g_pc_rewrite_seen = 1;
        reinterpret_cast<user_context_abi *>(ctx)->mcontext.pc =
            reinterpret_cast<unsigned long>(after_pc_rewrite);
    }
}

static int llsc_inc(volatile int *ptr)
{
    int result;
    __asm__ __volatile__(
        "ll.w %0, %1\n"
        "addi.w %0, %0, 1\n"
        "sc.w %0, %1\n"
        : "=&r"(result), "+ZC"(*ptr)
        :
        : "memory");
    return result;
}

static int run_llsc_loop(const char *tag)
{
    int stack_probe = 0;
    int global_sc = llsc_inc(&g_probe);
    int stack_sc = llsc_inc(&stack_probe);
    constexpr int k_llsc_iters = 1000000;
    for (int i = 1; i < k_llsc_iters; ++i)
    {
        if (llsc_inc(&g_probe) != 1)
        {
            printf("[llsc-exec-probe] %s global loop sc failed at i=%d val=%d\n", tag, i, g_probe);
            return 2;
        }
        if (llsc_inc(&stack_probe) != 1)
        {
            printf("[llsc-exec-probe] %s stack loop sc failed at i=%d val=%d\n", tag, i, stack_probe);
            return 3;
        }
    }
    printf("[llsc-exec-probe] %s sig=%d rewrite=%d global_sc=%d global_val=%d stack_sc=%d stack_val=%d\n",
           tag, g_sig_handled, g_pc_rewrite_seen, global_sc, g_probe, stack_sc, stack_probe);
    return (global_sc == 1 && g_probe == k_llsc_iters &&
            stack_sc == 1 && stack_probe == k_llsc_iters)
               ? 0
               : 1;
}

extern "C"
{
    __attribute__((noreturn)) void after_pc_rewrite()
    {
        exit(run_llsc_loop("pc-rewrite"));
    }

    __attribute__((section(".text.startup"))) int main()
    {
        kernel_sigaction_abi sa{};
        sa.handler = sigusr1_handler;
        sa.flags = SA_SIGINFO | SA_RESTART;
        if (raw_syscall4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, sizeof(kernel_sigset_t)) != 0)
        {
            printf("[llsc-exec-probe] rt_sigaction failed\n");
            exit(4);
        }
        if (raw_syscall2(SYS_tkill, raw_syscall0(SYS_gettid), SIGUSR1) != 0)
        {
            printf("[llsc-exec-probe] tkill failed\n");
            exit(5);
        }
        if (!g_sig_handled)
        {
            printf("[llsc-exec-probe] signal handler was not observed\n");
            exit(6);
        }
        g_probe = 0;
        if (run_llsc_loop("plain-signal") != 0)
        {
            exit(10);
        }

        kernel_sigaction_abi rewrite_sa{};
        rewrite_sa.handler = sigusr2_rewrite_handler;
        rewrite_sa.flags = SA_SIGINFO | SA_RESTART;
        if (raw_syscall4(SYS_rt_sigaction, SIGUSR2, (long)&rewrite_sa, 0, sizeof(kernel_sigset_t)) != 0)
        {
            printf("[llsc-exec-probe] rewrite rt_sigaction failed\n");
            exit(7);
        }
        g_probe = 0;
        if (raw_syscall2(SYS_tkill, raw_syscall0(SYS_gettid), SIGUSR2) != 0)
        {
            printf("[llsc-exec-probe] rewrite tkill failed\n");
            exit(8);
        }
        printf("[llsc-exec-probe] rewrite handler did not redirect control flow\n");
        exit(9);
    }
}

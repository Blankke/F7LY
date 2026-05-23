#include "user.hh"

static volatile int g_llsc_probe = 0;

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

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        int stack_llsc = 0;
        int global_sc = llsc_inc(&g_llsc_probe);
        int stack_sc = llsc_inc(&stack_llsc);
        printf("[llsc-probe] global_sc=%d global_val=%d stack_sc=%d stack_val=%d\n",
               global_sc, g_llsc_probe, stack_sc, stack_llsc);
        run_test("/llsc_exec_probe");

        // 临时先跑更窄的 pthread_tsd，确认问题是否已经缩到“普通线程退出”。
        if (chdir("/musl/") == 0)
        {
            char *argv[] = {
                (char *)"runtest.exe",
                (char *)"-w",
                (char *)"entry-static.exe",
                (char *)"pthread_tsd",
                NULL,
            };
            run_test("runtest.exe", argv, 0);
        }
        shutdown();
        return 0;
    }
}

#include "user.hh"

#define RUN_TESTS(test_list)                     \
    do                                           \
    {                                            \
        pid = fork();                            \
        if (pid < 0)                             \
        {                                        \
            printf("fork failed")                \
        }                                        \
        else if (pid == 0)                       \
        {                                        \
            if (execve(test_list, 0, 0) < 0)     \
            {                                    \
                printf("execve failed")          \
            }                                    \
            exit(0);                             \
        }                                        \
        else                                     \
        {                                        \
            int child_exit_state = -100;         \
            if (wait(-1, &child_exit_state) < 0) \
                printf("wait fail\n")            \
        }                                        \
    } while (0)

int basic_test(void);

extern "C"
{
    int main()
    {
        printf("Hello, World!\n");
        int a = 7;
        printf("a = %d\n", a);
        while (a--)
            ;
        printf("a = %d\n", a);

        chdir("/mnt/glibc/basic/");
        printf("#### OS COMP TEST GROUP START basic-glibc ####\n");

        basic_test();
        shutdown();
        return 0;
    }
}

int basic_test(void)
{
    [[maybe_unused]] int pid;
    // RUN_TESTS("write");
    // RUN_TESTS("fork");
    // RUN_TESTS("exit");
    // RUN_TESTS("wait");
    // RUN_TESTS("getpid");
    // RUN_TESTS("getppid");
    // RUN_TESTS("dup");
    // RUN_TESTS("dup2");
    // RUN_TESTS("execve");
    // RUN_TESTS("getcwd");
    // RUN_TESTS("gettimeofday");
    // RUN_TESTS("yield");
    // RUN_TESTS("sleep");
    // RUN_TESTS("times");
    // RUN_TESTS("clone");
    // RUN_TESTS("brk");
    // RUN_TESTS("waitpid");
    // RUN_TESTS("mmap");
    // RUN_TESTS("fstat");
    // RUN_TESTS("uname");
    // RUN_TESTS("openat");
    // RUN_TESTS("open");
    // RUN_TESTS("close");
    // RUN_TESTS("read");
    // RUN_TESTS("getdents");
    // RUN_TESTS("mkdir_");
    // RUN_TESTS("chdir");
    // RUN_TESTS("mount");
    // RUN_TESTS("umount");
    // RUN_TESTS("munmap");
    // RUN_TESTS("unlink");
    // RUN_TESTS("pipe");
    return 0;
}
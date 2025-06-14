
#include "user.hh"
int run_test(char *test_list)
{

    int pid = fork();
    if (pid < 0)
    {
        printf("fork failed");
    }
    else if (pid == 0)
    {
        if (execve(test_list, 0, 0) < 0)
        {
            printf("execve failed");
        }
        exit(0);
    }
    else
    {
        int child_exit_state = -100;
        if (wait(&child_exit_state) < 0)
            printf("wait fail\n");
    }
    return 0;
}

int basic_test(void)
{
    [[maybe_unused]] int pid;
    chdir("/mnt/musl/basic/");
    run_test("write");
    run_test("fork");
    run_test("exit");
    run_test("wait");
    run_test("getpid");
    run_test("getppid");
    run_test("dup");
    run_test("dup2");
    run_test("execve");
    run_test("getcwd");
    run_test("gettimeofday");
    run_test("yield");
    run_test("sleep");
    run_test("times");
    run_test("clone");
    run_test("brk");
    run_test("waitpid");
    run_test("mmap");
    run_test("fstat");
    run_test("uname");
    run_test("openat");
    run_test("open");
    run_test("close");
    run_test("read");
    run_test("getdents");
    run_test("mkdir_");
    run_test("chdir");
    run_test("mount");
    run_test("umount");
    run_test("munmap");
    run_test("unlink");
    run_test("pipe");
    return 0;
}

int start_shell(void)
{
    [[maybe_unused]] int pid;
    pid = fork();
    if (pid < 0)
    {
        printf("fork failed\n");
        return -1;
    }
    else if (pid == 0)
    {
        chdir("/mnt/musl/");
        char *bb_sh[8] = {0};
        bb_sh[0] = "busybox";
        bb_sh[1] = "sh";
        bb_sh[2] = "busybox_testcode.sh";
        printf("execve busybox shell\n");
        if (execve("busybox_unstripped", bb_sh, 0) < 0)
        {
            printf("execve failed\n");
            exit(1);
        }
        exit(0);
    }
    else
    {
        int child_exit_state = 33;
        if (wait(&child_exit_state) < 0)
            printf("wait fail\n");
        printf("shell exited with code %d\n", child_exit_state);
    }
    return 0;
}
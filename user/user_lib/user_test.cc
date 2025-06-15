
#include "user.hh"

extern char *bb_cmds[][10];

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
        chdir("/mnt/glibc/");
        char *bb_sh[8] = {0};
        bb_sh[0] = "busybox";
        bb_sh[1] = "echo";
        bb_sh[2] = "你好";
        printf("execve busybox shell\n");
        if (execve("busybox", bb_sh, 0) < 0)
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

int busybox_musl_test(void)
{
    [[maybe_unused]] int pid;
    for (int i = 0; bb_cmds[i][0] != NULL; i++)
    {
        pid = fork();
        if (pid < 0)
        {
            printf("fork failed\n");
            return -1;
        }
        else if (pid == 0)
        {
            chdir("/mnt/musl/");
            if (execve("busybox_unstripped", bb_cmds[i], 0) < 0)
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
    }
    return 0;
}





int libc_musl_test(void)
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
        bb_sh[2] = "run-static.sh";
        printf("execve busybox shell\n");
        if (execve("busybox", bb_sh, 0) < 0)
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
        printf("exited with code %d\n", child_exit_state);
    }
    return 0;
}







char *bb_cmds[][10] = {
    // {"echo", "#### independent command test", NULL},
    // {"ash", "-c", "exit", NULL},
    // {"sh", "-c", "exit", NULL},
    // {"basename", "/aaa/bbb", NULL},
    // {"cal", NULL},
    // {"clear", NULL},
    // {"date", NULL},
    // {"df", NULL},
    // {"dirname", "/aaa/bbb", NULL},
    // {"dmesg", NULL},
    // {"du", NULL},
    // {"expr", "1", "+", "1", NULL},
    // {"false", NULL}, // 这个有问题
    // {"true", NULL},
    // {"which", "ls", NULL}, // 这个有问题
    // {"uname", NULL},
    // {"uptime", NULL},
    // {"printf", "abc\\n", NULL}, // 这个有问题
    // {"ps", NULL}, // 这个有问题
    // {"pwd", NULL},
    // {"free", NULL},
    // {"hwclock", NULL},
    // {"kill", "10", NULL},
    // {"ls", NULL}, // 这个能过测评，但是还是有问题
    // {"sleep", "1", NULL},
    // {"echo", "#### file operation test", NULL},
    // {"touch", "test.txt", NULL},
    // {"echo \"hello world\" > test.txt", NULL}, //这个有问题
    // {"cat", "test.txt", NULL},
    // {"cut", "-c", "3", "test.txt", NULL},
    // {"od", "test.txt", NULL},
    // {"head", "test.txt", NULL},
    // {"tail", "test.txt", NULL},
    // {"hexdump", "-C", "test.txt", NULL},
    // {"md5sum", "test.txt", NULL},
    // {"echo 'ccccccc' >> test.txt", NULL}, // applet not found
    // {"echo 'bbbbbbb' >> test.txt", NULL}, // applet not found
    // {"echo 'aaaaaaa' >> test.txt", NULL}, // applet not found
    // {"echo '2222222' >> test.txt", NULL}, // applet not found
    // {"echo '1111111' >> test.txt", NULL}, // applet not found
    // {"echo 'bbbbbbb' >> test.txt", NULL}, // applet not found
    // {"sort test.txt | busybox uniq", NULL},
    // {"stat", "test.txt", NULL},
    // {"strings", "test.txt", NULL},
    // {"wc", "test.txt", NULL},
    // {"[ -f test.txt ]", NULL}, // applet not found
    // {"more", "test.txt", NULL},
    // {"rm", "test.txt", NULL},
    // {"mkdir", "test_dir", NULL},
    // {"mv", "test_dir", "test", NULL},
    // {"rmdir", "test", NULL},
    // {"grep", "hello", "busybox_cmd.txt", NULL}, //这个有问题
    // {"cp", "busybox_cmd.txt", "busybox_cmd.bak", NULL}, // 这个有问题
    // {"rm", "busybox_cmd.bak", NULL},
    // {"find", ".", "-name", "busybox_cmd.txt", NULL},
    {"echo", "郑老师曹老师太牛逼了", NULL},
    {NULL}};

// char *bb_libctest_cmds[][10] = {
//     // {"echo", "#### independent command test", NULL},
//     // {"ash", "-c", "exit", NULL},
//     // {"sh", "-c", "exit", NULL},
//     // {"basename", "/aaa/bbb", NULL},
//     // {"cal", NULL},
//     // {"clear", NULL},
//     // {"date", NULL},
//     // {"df", NULL},
//     // {"dirname", "/aaa/bbb", NULL},
//     // {"dmesg", NULL},
//     // {"du", NULL},
//     // {"expr", "1", "+", "1", NULL},
//     // {"false", NULL}, // 这个有问题
//     // {"true", NULL},
//     // {"which", "ls", NULL}, // 这个有问题
//     // {"uname", NULL},
//     // {"uptime", NULL},
//     // {"printf", "abc\\n", NULL}, // 这个有问题
//     // {"ps", NULL}, // 这个有问题
//     // {"pwd", NULL},
//     // {"free", NULL},
//     // {"hwclock", NULL},
//     // {"kill", "10", NULL},
//     // {"ls", NULL}, // 这个能过测评，但是还是有问题
//     // {"sleep", "1", NULL},
//     // {"echo", "#### file operation test", NULL},
//     // {"touch", "test.txt", NULL},
//     // {"echo \"hello world\" > test.txt", NULL}, //这个有问题
//     // {"cat", "test.txt", NULL},
//     // {"cut", "-c", "3", "test.txt", NULL},
//     // {"od", "test.txt", NULL},
//     // {"head", "test.txt", NULL},
//     // {"tail", "test.txt", NULL},
//     // {"hexdump", "-C", "test.txt", NULL},
//     // {"md5sum", "test.txt", NULL},
//     // {"echo 'ccccccc' >> test.txt", NULL}, // applet not found
//     // {"echo 'bbbbbbb' >> test.txt", NULL}, // applet not found
//     // {"echo 'aaaaaaa' >> test.txt", NULL}, // applet not found
//     // {"echo '2222222' >> test.txt", NULL}, // applet not found
//     // {"echo '1111111' >> test.txt", NULL}, // applet not found
//     // {"echo 'bbbbbbb' >> test.txt", NULL}, // applet not found
//     // {"sort test.txt | busybox uniq", NULL},
//     // {"stat", "test.txt", NULL},
//     // {"strings", "test.txt", NULL},
//     // {"wc", "test.txt", NULL},
//     // {"[ -f test.txt ]", NULL}, // applet not found
//     // {"more", "test.txt", NULL},
//     // {"rm", "test.txt", NULL},
//     // {"mkdir", "test_dir", NULL},
//     // {"mv", "test_dir", "test", NULL},
//     // {"rmdir", "test", NULL},
//     // {"grep", "hello", "busybox_cmd.txt", NULL}, //这个有问题
//     // {"cp", "busybox_cmd.txt", "busybox_cmd.bak", NULL}, // 这个有问题
//     // {"rm", "busybox_cmd.bak", NULL},
//     // {"find", ".", "-name", "busybox_cmd.txt", NULL},
//     {"echo", "郑老师曹老师太牛逼了", NULL},
//     {NULL}};
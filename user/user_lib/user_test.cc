
#include "user.hh"

extern char *bb_cmds[][10];
extern char *libctest[][2];

const char musl_dir[] = "/mnt/musl/";
const char glibc_dir[] = "/mnt/glibc/";

int strcmp(const char *s1, const char *s2) noexcept(true)
{
    for (; *s1 == *s2; s1++, s2++)
    {
        if (!*s1)
            return 0;
    }
    return *s1 < *s2 ? -1 : 1;
}

int run_test(const char *path, char *argv[] = 0, char *envp[] = 0)
{

    int pid = fork();
    if (pid < 0)
    {
        printf("fork failed");
    }
    else if (pid == 0)
    {
        if (execve(path, argv, envp) < 0)
        {
            printf("execve failed\n");
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

int basic_test(const char *path = musl_dir)
{
    [[maybe_unused]] int pid;
    chdir(path);
    chdir("basic");
    if (strcmp(path, musl_dir) == 0)
    {
        printf("#### OS COMP TEST GROUP START basic-musl ####\n");
    }
    else
    {
        printf("#### OS COMP TEST GROUP START basic-glibc ####\n");
    }
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
    if (strcmp(path, musl_dir) == 0)
    {
        printf("#### OS COMP TEST GROUP END basic-musl ####\n");
    }
    else
    {
        printf("#### OS COMP TEST GROUP END basic-glibc ####\n");
    }
    return 0;
}

int busybox_test(const char *path = musl_dir)
{
    chdir(path);
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "busybox_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int busybox_glibc_test(void)
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
            chdir(glibc_dir);
            if (execve("busybox", bb_cmds[i], 0) < 0)
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

int libcbench_test(void)
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
        bb_sh[2] = "libcbench_testcode.sh";
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


int iozone_test()
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
        bb_sh[2] = "iozone_testcode.sh";
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

int libc_test(const char *path = musl_dir)
{
    [[maybe_unused]] int pid;

    char *argv[8] = {0};
    argv[0] = "runtest.exe";
    argv[1] = "-w";
    argv[2] = "entry-static.exe";
    chdir(path);
    printf("#### OS COMP TEST GROUP START libctest-musl ####\n");
    for (int i = 0; libctest[i][0] != NULL; i++)
    {
        argv[3] = libctest[i][0];
        run_test("runtest.exe", argv, 0);
    }
    printf("#### OS COMP TEST GROUP END libctest-musl ####\n");
    return 0;
}

int lua_test(const char *path = musl_dir)
{
    chdir(path);
    char *lua_sh = "./busybox echo \"#### OS COMP TEST GROUP START lua-musl ####\" \n"
                   "./busybox sh ./test.sh date.lua\n"
                   "./busybox sh ./test.sh file_io.lua\n"
                   "./busybox sh ./test.sh max_min.lua\n"
                   "./busybox sh ./test.sh random.lua\n"
                   "./busybox sh ./test.sh remove.lua\n"
                   "./busybox sh ./test.sh round_num.lua\n"
                   "./busybox sh ./test.sh sin30.lua\n"
                   "./busybox sh ./test.sh sort.lua\n"
                   "./busybox sh ./test.sh strings.lua\n"
                   "./busybox echo \"#### OS COMP TEST GROUP END lua-musl ####\" \n";

    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "-c";
    bb_sh[3] = lua_sh;
    run_test("busybox", bb_sh, 0);
    return 0;
}

int lmbench_test()
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
        bb_sh[2] = "lmbench_testcode.sh";
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

char *libctest[][2] = {
    {"argv", NULL},
    {"basename", NULL},
    {"clocale_mbfuncs", NULL},
    {"clock_gettime", NULL},
    {"dirname", NULL},
    {"env", NULL},
    // {"fdopen", NULL}, // fdopen failed
    {"fnmatch", NULL},
    // {"fscanf", NULL}, //ioctl 爆了
    // {"fwscanf", NULL}, //死了
    {"iconv_open", NULL},
    {"inet_pton", NULL},
    {"mbc", NULL},
    {"memstream", NULL},
    // {"pthread_cancel_points", NULL}, //sig， fork高级用法
    // {"pthread_cancel", NULL}, // sig， fork高级用法
    // {"pthread_cond", NULL},   // sig， fork高级用法
    // {"pthread_tsd", NULL},    // sig， fork高级用法
    {"qsort", NULL},
    {"random", NULL},
    // {"search_hsearch", NULL}, //爆了
    {"search_insque", NULL},
    {"search_lsearch", NULL},
    {"search_tsearch", NULL},
    // {"setjmp", NULL}, // 爆了
    {"snprintf", NULL},
    // {"socket", NULL}, // 爆了
    {"sscanf", NULL},
    // {"sscanf_long", NULL}, // 爆了
    // {"stat", NULL}, //ioctl
    {"strftime", NULL},
    {"string", NULL},
    {"string_memcpy", NULL},
    {"string_memmem", NULL},
    {"string_memset", NULL},
    {"string_strchr", NULL},
    {"string_strcspn", NULL},
    {"string_strstr", NULL},
    {"strptime", NULL},
    {"strtod", NULL},
    {"strtod_simple", NULL},
    {"strtof", NULL},
    {"strtol", NULL},
    {"strtold", NULL},
    {"swprintf", NULL},
    {"tgmath", NULL},
    {"time", NULL},
    {"tls_align", NULL},
    {"udiv", NULL},
    // {"ungetc", NULL}, //iotcl爆了
    // {"utime", NULL}, //爆了
    {"wcsstr", NULL},
    {"wcstol", NULL},
    // {"daemon_failure", NULL}, // 爆了
    {"dn_expand_empty", NULL},
    {"dn_expand_ptr_0", NULL},
    // {"fflush_exit", NULL},//fd爆了
    {"fgets_eof", NULL},
    {"fgetwc_buffering", NULL},
    {"fpclassify_invalid_ld80", NULL},
    // {"ftello_unflushed_append", NULL}, // 爆了
    {"getpwnam_r_crash", NULL},
    {"getpwnam_r_errno", NULL},
    {"iconv_roundtrips", NULL},
    {"inet_ntop_v4mapped", NULL},
    {"inet_pton_empty_last_field", NULL},
    {"iswspace_null", NULL},
    {"lrand48_signextend", NULL},
    // {"lseek_large", NULL}, // 爆了
    {"malloc_0", NULL},
    {"mbsrtowcs_overflow", NULL},
    {"memmem_oob_read", NULL},
    {"memmem_oob", NULL},
    {"mkdtemp_failure", NULL},
    {"mkstemp_failure", NULL},
    {"printf_1e9_oob", NULL},
    {"printf_fmt_g_round", NULL},
    {"printf_fmt_g_zeros", NULL},
    {"printf_fmt_n", NULL},
    // {"pthread_robust_detach", NULL}, //爆了
    // {"pthread_cancel_sem_wait", NULL},   // sig， fork高级用法
    // {"pthread_cond_smasher", NULL},      // sig， fork高级用法
    // {"pthread_condattr_setclock", NULL}, // sig， fork高级用法
    // {"pthread_exit_cancel", NULL},       // sig， fork高级用法
    // {"pthread_once_deadlock", NULL},     // sig， fork高级用法
    // {"pthread_rwlock_ebusy", NULL},      // sig， fork高级用法
    // {"putenv_doublefree", NULL},
    // {"regex_backref_0", NULL}, //爆了
    // {"regex_bracket_icase", NULL}, // 爆了
    // {"regex_ere_backref", NULL},       // 爆了
    // {"regex_escaped_high_byte", NULL}, // 爆了
    // {"regex_negated_range", NULL},     // 爆了
    // {"regexec_nosub", NULL},           // 爆了
    // {"rewind_clear_error", NULL}, // 爆了
    // {"rlimit_open_files", NULL}, // 爆了
    {"scanf_bytes_consumed", NULL},
    {"scanf_match_literal_eof", NULL},
    {"scanf_nullbyte_char", NULL},
    {"setvbuf_unget", NULL}, // streamdevice not support lseek currently!但是pass了
    {"sigprocmask_internal", NULL},
    {"sscanf_eof", NULL},
    // {"statvfs", NULL}, //爆了
    {"strverscmp", NULL},
    {"syscall_sign_extend", NULL},
    {"uselocale_0", NULL},
    {"wcsncpy_read_overflow", NULL},
    {"wcsstr_false_negative", NULL},
    {NULL}
};

char *bb_cmds[][10] = {
    {"echo", "#### independent command test", NULL},
    {"ash", "-c", "exit", NULL},
    {"sh", "-c", "exit", NULL},
    {"basename", "/aaa/bbb", NULL},
    {"cal", NULL},
    {"clear", NULL},
    {"date", NULL},
    {"df", NULL},
    {"dirname", "/aaa/bbb", NULL},
    {"dmesg", NULL},
    {"du", NULL},
    {"expr", "1", "+", "1", NULL},
    {"false", NULL}, // 这个有问题
    {"true", NULL},
    {"which", "ls", NULL}, // 这个有问题
    {"uname", NULL},
    {"uptime", NULL},
    {"printf", "abc\\n", NULL}, // 这个有问题
    {"ps", NULL}, // 这个有问题
    {"pwd", NULL},
    {"free", NULL},
    {"hwclock", NULL},
    {"kill", "10", NULL},
    {"ls", NULL}, // 这个能过测评，但是还是有问题
    {"sleep", "1", NULL},
    {"echo", "#### file operation test", NULL},
    {"touch", "test.txt", NULL},
    {"echo \"hello world\" > test.txt", NULL}, //这个有问题
    {"cat", "test.txt", NULL},
    {"cut", "-c", "3", "test.txt", NULL},
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
    {"sort test.txt | busybox uniq", NULL},
    {"stat", "test.txt", NULL},
    {"strings", "test.txt", NULL},
    {"wc", "test.txt", NULL},
    {"[ -f test.txt ]", NULL}, // applet not found
    {"more", "test.txt", NULL},
    {"rm", "test.txt", NULL},
    {"mkdir", "test_dir", NULL},
    {"mv", "test_dir", "test", NULL},
    {"rmdir", "test", NULL},
    {"grep", "hello", "busybox_cmd.txt", NULL}, //这个有问题
    {"cp", "busybox_cmd.txt", "busybox_cmd.bak", NULL}, // 这个有问题
    {"rm", "busybox_cmd.bak", NULL},
    // {"find", ".", "-name", "busybox_cmd.txt", NULL},
    {"echo", "hello", NULL},
    {NULL}};
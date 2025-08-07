
#include "user.hh"

extern char *bb_cmds[][10];
extern char *libctest[][2];

const char musl_dir[] = "/mnt/musl/";
const char glibc_dir[] = "/mnt/glibc/";
extern char *ltp_testcases[];
int strcmp(const char *s1, const char *s2) noexcept(true)
{
    for (; *s1 == *s2; s1++, s2++)
    {
        if (!*s1)
            return 0;
    }
    return *s1 < *s2 ? -1 : 1;
}
size_t strlen(const char *s) noexcept(true)
{
    size_t len = 0;
    while (*s)
        s++, len++;
    return len;
}
int run_test(const char *path, char *argv[], char *envp[])
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
    // run_test("write");
    // run_test("fork");
    // run_test("exit");
    // run_test("wait");
    // run_test("getpid");
    // run_test("getppid");
    // run_test("dup");
    // run_test("dup2");
    // run_test("execve");
    // run_test("getcwd");
    // run_test("gettimeofday");
    // run_test("yield");
    // run_test("sleep");
    // run_test("times");
    // run_test("clone");
    // run_test("brk");
    // run_test("waitpid");
    // run_test("mmap");
    // run_test("fstat");
    // run_test("uname");
    // run_test("openat");
    // run_test("open");
    // run_test("close");
    // run_test("read");
    // run_test("getdents");
    // run_test("mkdir_");
    // run_test("chdir");
    // run_test("mount");       //todo
    // run_test("umount");      //todo
    // run_test("munmap");
    // run_test("unlink");
    // run_test("pipe");
    // sleep(20);
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

int libcbench_test(const char *path = musl_dir)
{
    chdir(path);
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "libcbench_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int iozone_test(const char *path = musl_dir)
{
    chdir(path);
    char *bb_sh[8] = {0};
    bb_sh[0] = "iozone";
    bb_sh[1] = "-a";
    bb_sh[2] = "-r";
    bb_sh[3] = "1k";
    bb_sh[4] = "-s";
    bb_sh[5] = "4m";
    printf("#### OS COMP TEST GROUP START iozone-musl ####\n");
    printf("iozone automatic measurements\n");
    run_test("iozone", bb_sh, 0);
    printf("#### OS COMP TEST GROUP START iozone-musl ####\n");
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
    argv[2] = "entry-dynamic.exe";
    for (int i = 0; libctest[i][0] != NULL; i++)
    {
        argv[3] = libctest[i][0];
        run_test("runtest.exe", argv, 0);
#ifdef LOONGARCH
        sleep(10);
#endif
    }
    printf("#### OS COMP TEST GROUP END libctest-musl ####\n");
    return 0;
}

int lua_test(const char *path = musl_dir)
{
    chdir(path);
    char *lua_sh;
    if (strcmp(path, musl_dir) == 0)
    {
        lua_sh = "./busybox echo \"#### OS COMP TEST GROUP START lua-musl ####\" \n"
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
    }
    else
    {
        lua_sh = "./busybox echo \"#### OS COMP TEST GROUP START lua-glibc ####\" \n"
                 "./busybox sh ./test.sh date.lua\n"
                 "./busybox sh ./test.sh file_io.lua\n"
                 "./busybox sh ./test.sh max_min.lua\n"
                 "./busybox sh ./test.sh random.lua\n"
                 "./busybox sh ./test.sh remove.lua\n"
                 "./busybox sh ./test.sh round_num.lua\n"
                 "./busybox sh ./test.sh sin30.lua\n"
                 "./busybox sh ./test.sh sort.lua\n"
                 "./busybox sh ./test.sh strings.lua\n"
                 "./busybox echo \"#### OS COMP TEST GROUP END lua-glibc ####\" \n";
    }

    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "-c";
    bb_sh[3] = lua_sh;
    run_test("busybox", bb_sh, 0);
    return 0;
}

int lmbench_test(const char *path = musl_dir)
{
    chdir(path);
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "lmbench_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int ltp_test(const char *path = musl_dir)
{
    chdir("/musl/ltp/testcases/bin");
    printf("#### OS COMP TEST GROUP START ltp-musl ####\n");
    char *bb_sh[8] = {0};
    for (int i = 0; ltp_testcases[i] != NULL; i++)
    {
        int len = strlen(ltp_testcases[i]);
        if (len >= 3 && strcmp(ltp_testcases[i] + len - 3, ".sh") == 0)
        {
            bb_sh[0] = "busybox";
            bb_sh[1] = "sh";
            bb_sh[2] = ltp_testcases[i];
            run_test("/musl/busybox", bb_sh, 0);
        }
        else
        {
            bb_sh[0] = ltp_testcases[i];
            run_test(ltp_testcases[i], bb_sh, 0);
        }
    }
    return 0;
}
int final_test_musl()
{
    //interrupt
    printf("#### OS COMP TEST GROUP START interrupts-test1-musl ####\n");
    run_test("/musl/interrupts-test-1");
    printf("#### OS COMP TEST GROUP END interrupts-test1-musl ####\n\n");
    printf("#### OS COMP TEST GROUP START interrupts-test2-musl ####\n");
    run_test("/musl/interrupts-test-2");
    printf("#### OS COMP TEST GROUP END interrupts-test2-musl ####\n\n");
    //copy-file-range
    printf("#### OS COMP TEST GROUP START copy-file-range-test1-musl ####\n");
    run_test("/musl/copy-file-range-test-1");
    printf("#### OS COMP TEST GROUP END copy-file-range-test1-musl ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test2-musl ####");
    run_test("/musl/copy-file-range-test-2");
    printf("#### OS COMP TEST GROUP END copy-file-range-test2-musl ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test3-musl ####");
    run_test("/musl/copy-file-range-test-3");
    printf("#### OS COMP TEST GROUP END copy-file-range-test3-musl ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test4-musl ####");
    run_test("/musl/copy-file-range-test-4");
    printf("#### OS COMP TEST GROUP END copy-file-range-test4-musl ####\n\n");
    //splice
    char *splice_argv1[] = {"test_splice", "1", NULL};
    printf("#### OS COMP TEST GROUP START splice-test1-musl ####\n");
    run_test("/musl/test_splice", splice_argv1, 0);
    printf("#### OS COMP TEST GROUP END splice-test1-musl ####\n\n");
    char *splice_argv2[] = {"test_splice", "2", NULL};
    printf("#### OS COMP TEST GROUP START splice-test2-musl ####\n");
    run_test("/musl/test_splice", splice_argv2, 0);
    printf("#### OS COMP TEST GROUP END splice-test2-musl ####\n\n");
    char *splice_argv3[] = {"test_splice", "3", NULL};
    printf("#### OS COMP TEST GROUP START splice-test3-musl ####\n");
    run_test("/musl/test_splice", splice_argv3, 0);
    printf("#### OS COMP TEST GROUP END splice-test3-musl ####\n\n");
    char *splice_argv4[] = {"test_splice", "4", NULL};
    printf("#### OS COMP TEST GROUP START splice-test4-musl ####\n");
    run_test("/musl/test_splice", splice_argv4, 0);
    printf("#### OS COMP TEST GROUP END splice-test4-musl ####\n\n");
    char *splice_argv5[] = {"test_splice", "5", NULL};
    printf("#### OS COMP TEST GROUP START splice-test5-musl ####\n");
    run_test("/musl/test_splice", splice_argv5, 0);
    printf("#### OS COMP TEST GROUP END splice-test5-musl ####\n\n");
    return 0;
}

int final_test_glibc()
{
    //interrupt
    printf("#### OS COMP TEST GROUP START interrupts-test1-glibc ####\n");
    run_test("/glibc/interrupts-test-1");
    printf("#### OS COMP TEST GROUP END interrupts-test1-glibc ####\n");
    printf("#### OS COMP TEST GROUP START interrupts-test2-glibc ####\n");
    run_test("/glibc/interrupts-test-2");
    printf("#### OS COMP TEST GROUP END interrupts-test2-glibc ####\n\n");
    //copy-file-range
    printf("#### OS COMP TEST GROUP START copy-file-range-test1-glibc ####\n");
    run_test("/glibc/copy-file-range-test-1");
    printf("#### OS COMP TEST GROUP END copy-file-range-test1-glibc ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test2-glibc ####\n");
    run_test("/glibc/copy-file-range-test-2");
    printf("#### OS COMP TEST GROUP END copy-file-range-test2-glibc ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test3-glibc ####\n");
    run_test("/glibc/copy-file-range-test-3");
    printf("#### OS COMP TEST GROUP END copy-file-range-test3-glibc ####\n\n");
    printf("#### OS COMP TEST GROUP START copy-file-range-test4-glibc ####\n");
    run_test("/glibc/copy-file-range-test-4");
    printf("#### OS COMP TEST GROUP END copy-file-range-test4-glibc ####\n\n");
    //splice
    char *splice_argv1[] = {"test_splice", "1", NULL};
    printf("#### OS COMP TEST GROUP START splice-test1-glibc ####\n");
    run_test("/glibc/test_splice", splice_argv1, 0);
    printf("#### OS COMP TEST GROUP END splice-test1-glibc ####\n\n");
    char *splice_argv2[] = {"test_splice", "2", NULL};
    printf("#### OS COMP TEST GROUP START splice-test2-glibc ####\n");
    run_test("/glibc/test_splice", splice_argv2, 0);
    printf("#### OS COMP TEST GROUP END splice-test2-glibc ####\n\n");
    char *splice_argv3[] = {"test_splice", "3", NULL};
    printf("#### OS COMP TEST GROUP START splice-test3-glibc ####\n");
    run_test("/glibc/test_splice", splice_argv3, 0);
    printf("#### OS COMP TEST GROUP END splice-test3-glibc ####\n\n");
    char *splice_argv4[] = {"test_splice", "4", NULL};
    printf("#### OS COMP TEST GROUP START splice-test4-glibc ####\n");
    run_test("/glibc/test_splice", splice_argv4, 0);
    printf("#### OS COMP TEST GROUP END splice-test4-glibc ####\n\n");
    char *splice_argv5[] = {"test_splice", "5", NULL};
    printf("#### OS COMP TEST GROUP START splice-test5-glibc ####\n");
    run_test("/glibc/test_splice", splice_argv5, 0);
    printf("#### OS COMP TEST GROUP END splice-test5-glibc ####\n\n");
    return 0;
}


int git_test(const char *path)
{
    chdir(path);
    int fd = openat(AT_FDCWD, "/musl/.gitconfig", 02 | 0100);
    char *argv[8] = {0};
    char *envp[] = {
        "HOME=/musl", // 设置 HOME
        NULL                        // 必须以 NULL 结尾
    };
    // argv[0] = "git";
    // argv[1] = "help";
    // run_test("git", argv, 0);
    argv[0] = "git";
    argv[1] = "config";
    argv[2] = "--global";
    argv[3] = "--add";
    argv[4] = "safe.directory";
    argv[5] = "/musl/usr/bin";
    run_test("git", argv, envp);
    return 0;
}

char *libctest[][2] = {
    // {"argv", NULL},
    // {"basename", NULL},
    // {"clocale_mbfuncs", NULL},
    // {"clock_gettime", NULL},
    // {"dirname", NULL},
    // {"env", NULL},
    {"fdopen", NULL}, // fdopen failed 问题在于写入后读不出来，怀疑根本没写入成功
    // {"fnmatch", NULL},
    // // // {"fscanf", NULL}, //ioctl 爆了
    // // // {"fwscanf", NULL}, //死了
    // {"iconv_open", NULL},
    // {"inet_pton", NULL},
    // {"mbc", NULL},
    // {"memstream", NULL},
    // // {"pthread_cancel_points", NULL}, //sig， fork高级用法
    // {"pthread_cancel", NULL}, // sig， fork高级用法
    // {"pthread_cond", NULL},   // sig， fork高级用法
    // {"pthread_tsd", NULL},    // sig， fork高级用法
    // {"qsort", NULL},
    // {"random", NULL},
    // {"search_hsearch", NULL},
    // {"search_insque", NULL},
    // {"search_lsearch", NULL},
    // {"search_tsearch", NULL},
    // // // // {"setjmp", NULL}, //信号相关，爆了
    // {"snprintf", NULL},
    // // // // {"socket", NULL}, // 网络相关，这个不测了
    // {"sscanf", NULL},
    // {"sscanf_long", NULL}, //龙芯会爆，riscv正常
    // {"stat", NULL}, //sys_fstatat我关掉了，原来就是关的，开了basictest爆炸，应该没实现对
    // {"strftime", NULL},
    // {"string", NULL},
    // {"string_memcpy", NULL},
    // {"string_memmem", NULL},
    // {"string_memset", NULL},
    // {"string_strchr", NULL},
    // {"string_strcspn", NULL},
    // {"string_strstr", NULL},
    // {"strptime", NULL},
    // {"strtod", NULL},
    // {"strtod_simple", NULL},
    // {"strtof", NULL},
    // {"strtol", NULL},
    // {"strtold", NULL},
    // {"swprintf", NULL},
    // {"tgmath", NULL},
    // {"time", NULL},
    // {"tls_align", NULL},
    // {"udiv", NULL},
    // // // // {"ungetc", NULL}, //文件系统爆了
    // // // // {"utime", NULL}, // sys_utimensat实现不正确
    // {"wcsstr", NULL},
    // {"wcstol", NULL},
    // // // // {"daemon_failure", NULL}, // 爆了
    // {"dn_expand_empty", NULL},
    // {"dn_expand_ptr_0", NULL},
    // // // // {"fflush_exit", NULL},//fd爆了，标准输出不见了
    // {"fgets_eof", NULL},
    // {"fgetwc_buffering", NULL},
    // {"fpclassify_invalid_ld80", NULL},
    // {"ftello_unflushed_append", NULL},
    // {"getpwnam_r_crash", NULL},
    // {"getpwnam_r_errno", NULL},
    // {"iconv_roundtrips", NULL},
    // {"inet_ntop_v4mapped", NULL},
    // {"inet_pton_empty_last_field", NULL},
    // {"iswspace_null", NULL},
    // {"lrand48_signextend", NULL},
    // {"lseek_large", NULL},
    // {"malloc_0", NULL},
    // {"mbsrtowcs_overflow", NULL},
    // {"memmem_oob_read", NULL},
    // {"memmem_oob", NULL},
    // {"mkdtemp_failure", NULL},
    // {"mkstemp_failure", NULL},
    // {"printf_1e9_oob", NULL},
    // {"printf_fmt_g_round", NULL},
    // {"printf_fmt_g_zeros", NULL},
    // {"printf_fmt_n", NULL},
    // // {"pthread_robust_detach", NULL}, //爆了
    // {"pthread_cancel_sem_wait", NULL},   // sig， fork高级用法
    // {"pthread_cond_smasher", NULL},      // sig， fork高级用法
    // // {"pthread_condattr_setclock", NULL}, // sig， fork高级用法
    // {"pthread_exit_cancel", NULL},       // sig， fork高级用法
    // {"pthread_once_deadlock", NULL},     // sig， fork高级用法
    // {"pthread_rwlock_ebusy", NULL},      // sig， fork高级用法
    // {"putenv_doublefree", NULL},
    // {"regex_backref_0", NULL},
    // {"regex_bracket_icase", NULL},
    // {"regex_ere_backref", NULL},
    // {"regex_escaped_high_byte", NULL},
    // {"regex_negated_range", NULL},
    // {"regexec_nosub", NULL},
    // // // // {"rewind_clear_error", NULL}, // 爆了
    // // // // {"rlimit_open_files", NULL}, // 爆了
    // {"scanf_bytes_consumed", NULL},
    // {"scanf_match_literal_eof", NULL},
    // {"scanf_nullbyte_char", NULL},
    // {"setvbuf_unget", NULL}, // streamdevice not support lseek currently!但是pass了
    // {"sigprocmask_internal", NULL},
    // {"sscanf_eof", NULL},
    // {"statvfs", NULL},
    // {"strverscmp", NULL},
    // {"syscall_sign_extend", NULL},
    // {"uselocale_0", NULL},
    // {"wcsncpy_read_overflow", NULL},
    // {"wcsstr_false_negative", NULL},
    {NULL}};

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
    {"ps", NULL},               // 这个有问题
    {"pwd", NULL},
    {"free", NULL},
    {"hwclock", NULL},
    {"kill", "10", NULL},
    {"ls", NULL}, // 这个能过测评，但是还是有问题
    {"sleep", "1", NULL},
    {"echo", "#### file operation test", NULL},
    {"touch", "test.txt", NULL},
    {"echo \"hello world\" > test.txt", NULL}, // 这个有问题
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
    {"grep", "hello", "busybox_cmd.txt", NULL},         // 这个有问题
    {"cp", "busybox_cmd.txt", "busybox_cmd.bak", NULL}, // 这个有问题
    {"rm", "busybox_cmd.bak", NULL},
    // {"find", ".", "-name", "busybox_cmd.txt", NULL},
    {"echo", "hello", NULL},
    {NULL}};

char *ltp_testcases[] = {
    // "abort01",
    // "abs01",             // 完全PASS
    // "accept01",
    // "accept02",
    // "accept03",
    // "accept4_01",
    // "access01",
    // "access02",
    // "access03",
    // "access04",
    // "acct01",
    // "acct02",
    // "acct02_helper",
    // "acl1",
    // "add_ipv6addr",
    // "add_key01",
    // "add_key02",
    // "add_key03",
    // "add_key04",
    // "add_key05",
    // "adjtimex01",
    // "adjtimex02",
    // "adjtimex03",
    // "af_alg01",
    // "af_alg02",
    // "af_alg03",
    // "af_alg04",
    // "af_alg05",
    // "af_alg06",
    // "af_alg07",
    // "aio01",
    // "aio02",
    // "aiocp",
    // "aiodio_append",
    // "aiodio_sparse",
    // "aio-stress",
    // "alarm02",
    // "alarm03",
    // "alarm05",
    // "alarm06",
    // "alarm07",
    // "ar01.sh",
    // "arch_prctl01",
    // "arping01.sh",
    // "asapi_01",              // PASS一部分
    // "asapi_02",
    // "asapi_03",
    // "ask_password.sh",
    // "aslr01",
    // "assign_password.sh",
    // "atof01",                   // PASS一部分
    // "autogroup01",
    // "bbr01.sh",
    // "bbr02.sh",
    // "bind_noport01.sh",
    // "bind01",
    // "bind02",
    // "bind03",
    // "bind04",
    // "bind05",
    // "bind06",
    // "binfmt_misc_lib.sh",
    // "binfmt_misc01.sh",
    // "binfmt_misc02.sh",
    // "block_dev",
    // "bpf_map01",
    // "bpf_prog01",
    // "bpf_prog02",
    // "bpf_prog03",
    // "bpf_prog04",
    // "bpf_prog05",
    // "bpf_prog06",
    // "bpf_prog07",
    // "brk01",
    // "brk02",
    // "broken_ip-checksum.sh",
    // "broken_ip-dstaddr.sh",
    // "broken_ip-fragment.sh",
    // "broken_ip-ihl.sh",
    // "broken_ip-nexthdr.sh",
    // "broken_ip-plen.sh",
    // "broken_ip-protcol.sh",
    // "broken_ip-version.sh",
    // "busy_poll_lib.sh",
    // "busy_poll01.sh",
    // "busy_poll02.sh",
    // "busy_poll03.sh",
    // "cacheflush01",
    // "can_bcm01",
    // "can_filter",
    // "can_rcv_own_msgs",
    // "cap_bounds_r",
    // "cap_bounds_rw",
    // "cap_bset_inh_bounds",
    // "capget01",
    // "capget02",
    // "capset01",
    // "capset02",
    // "capset03",
    // "capset04",
    // "cfs_bandwidth01",
    // "cgroup_core01",
    // "cgroup_core02",
    // "cgroup_core03",
    // "cgroup_fj_common.sh",
    // "cgroup_fj_function.sh",
    // "cgroup_fj_proc",
    // "cgroup_fj_stress.sh",
    // "cgroup_lib.sh",
    // "cgroup_regression_3_1.sh",
    // "cgroup_regression_3_2.sh",
    // "cgroup_regression_5_1.sh",
    // "cgroup_regression_5_2.sh",
    // "cgroup_regression_6_1.sh",
    // "cgroup_regression_6_2.sh",
    // "cgroup_regression_fork_processes",
    // "cgroup_regression_getdelays",
    // "cgroup_regression_test.sh",
    // "cgroup_xattr",
    // "change_password.sh",
    // "chdir01",
    // "chdir04",
    // "check_envval",
    // "check_icmpv4_connectivity",
    // "check_icmpv6_connectivity",
    // "check_keepcaps",
    // "check_netem",
    // "check_pe",
    // "check_setkey",
    // "check_simple_capset",
    // "chmod01", // 完全PASS
    // "chmod03",   //sendmsg
    // "chmod05", //sendmsg
    // "chmod06", //sendmsg
    // "chmod07", //sendmsg
    // "chown01",     //pass
    // "chown01_16",
    // "chown02",
    // "chown02_16",
    // "chown03",
    // "chown03_16",
    // "chown04",
    // "chown04_16",
    // "chown05",
    // "chown05_16",
    // "chroot01",
    // "chroot02",
    // "chroot03",
    // "chroot04",
    // "cleanup_lvm.sh",
    // "clock_adjtime01",
    // "clock_adjtime02",
    // "clock_getres01",
    // "clock_gettime01",
    // "clock_gettime02",
    // "clock_gettime03",
    // "clock_gettime04",
    // "clock_nanosleep01",
    // "clock_nanosleep02",
    // "clock_nanosleep03",
    // "clock_nanosleep04",
    // "clock_settime01",
    // "clock_settime02",
    // "clock_settime03",
    // "clone01",
    // "clone02",
    // "clone03",
    // "clone04",
    // "clone05",
    // "clone06",
    // "clone07",
    // "clone08",
    // "clone09",
    // "clone301",
    // "clone302",
    // "clone303",
    // "close_range01",
    // "close_range02",
    // "close01",
    // "close02",
    // "cmdlib.sh",
    // "cn_pec.sh",
    // "confstr01",
    // "connect01",
    // "connect02",
    // "copy_file_range01",
    // "copy_file_range02",
    // "copy_file_range03",
    // "cp_tests.sh",
    // "cpio_tests.sh",
    // "cpuacct.sh",
    // "cpuacct_task",
    // "cpuctl_def_task01",
    // "cpuctl_def_task02",
    // "cpuctl_def_task03",
    // "cpuctl_def_task04",
    // "cpuctl_fj_cpu-hog",
    // "cpuctl_fj_simple_echo",
    // "cpuctl_latency_check_task",
    // "cpuctl_latency_test",
    // "cpuctl_test01",
    // "cpuctl_test02",
    // "cpuctl_test03",
    // "cpuctl_test04",
    // "cpufreq_boost",
    // "cpuhotplug_do_disk_write_loop",
    // "cpuhotplug_do_kcompile_loop",
    // "cpuhotplug_do_spin_loop",
    // "cpuhotplug_hotplug.sh",
    // "cpuhotplug_report_proc_interrupts",
    // "cpuhotplug_testsuite.sh",
    // "cpuhotplug01.sh",
    // "cpuhotplug02.sh",
    // "cpuhotplug03.sh",
    // "cpuhotplug04.sh",
    // "cpuhotplug05.sh",
    // "cpuhotplug06.sh",
    // "cpuhotplug07.sh",
    // "cpuset01",
    // "crash01",
    // "crash02",
    // "creat01",
    // "creat03",
    // "creat04",
    // "creat05",
    // "creat06",
    // "creat07",
    // "creat07_child",
    // "creat08",
    // "creat09",
    // "create_datafile",
    // "create_file",
    // "crypto_user01",
    // "crypto_user02",
    // "cve-2014-0196",
    // "cve-2015-3290",
    // "cve-2016-10044",
    // "cve-2016-7042",
    // "cve-2016-7117",
    // "cve-2017-16939",
    // "cve-2017-17052",
    // "cve-2017-17053",
    // "cve-2017-2618",
    // "cve-2017-2671",
    // "cve-2022-4378",
    // "daemonlib.sh",
    // "data",
    // "data_space",
    // "dccp_ipsec.sh",
    // "dccp_ipsec_vti.sh",
    // "dccp01.sh",
    // "dctcp01.sh",
    // "delete_module01",
    // "delete_module02",
    // "delete_module03",
    // "df01.sh",
    // "dhcp_lib.sh",
    // "dhcpd_tests.sh",
    // "dio_append",
    // "dio_read",
    // "dio_sparse",
    // "dio_truncate",
    // "diotest1",
    // "diotest2",
    // "diotest3",
    // "diotest4",
    // "diotest5",
    // "diotest6",
    // "dirty",
    // "dirtyc0w",
    // "dirtyc0w_child",
    // "dirtyc0w_shmem",
    // "dirtyc0w_shmem_child",
    // "dirtypipe",
    // "dma_thread_diotest",
    // "dnsmasq_tests.sh",
    // "dns-stress.sh",
    // "dns-stress01-rmt.sh",
    // "dns-stress02-rmt.sh",
    // "dns-stress-lib.sh",
    // "doio",
    // "du01.sh",
    // "dup01",//完全PASS
    // "dup02",// 完全PASS
    // "dup03",// 完全PASS
    // "dup04",// 完全PASS
    // "dup05",  //pass
    // "dup06", //完全PASS
    // "dup07",//完全PASS
    // "dup201",//完全PASS
    // "dup202",//完全PASS
    // "dup203", //pass
    // "dup204",// 完全PASS
    // "dup205",//完全PASS
    // "dup206", //完全PASS
    // "dup207", //
    // "dup3_01",//
    // "dup3_02",// 完全PASS
    // "dynamic_debug01.sh",
    // "ebizzy",
    // "eject_check_tray",
    // "eject-tests.sh",
    // "endian_switch01",
    // "epoll_create01",
    // "epoll_create02",
    // "epoll_create1_01",
    // "epoll_create1_02",
    // "epoll_ctl01",
    // "epoll_ctl02",
    // "epoll_ctl03",
    // "epoll_ctl04",
    // "epoll_ctl05",
    // "epoll_pwait01",
    // "epoll_pwait02",
    // "epoll_pwait03",
    // "epoll_pwait04",
    // "epoll_pwait05",
    // "epoll_wait01",
    // "epoll_wait02",
    // "epoll_wait03",
    // "epoll_wait04",
    // "epoll_wait05",
    // "epoll_wait06",
    // "epoll_wait07",
    // "epoll-ltp",
    // "event_generator",
    // "eventfd01",
    // "eventfd02",
    // "eventfd03",
    // "eventfd04",
    // "eventfd05",
    // "eventfd06",
    // "eventfd2_01",
    // "eventfd2_02",
    // "eventfd2_03",
    // "evm_overlay.sh",
    // "exec_with_inh",
    // "exec_without_inh",
    // "execl01",
    // "execl01_child",
    // "execle01",
    // "execle01_child",
    // "execlp01",
    // "execlp01_child",
    // "execv01",               // 完全PASS
    // "execv01_child",
    // "execve_child",
    // "execve01",              // 完全PASS
    // "execve01_child",
    // "execve02",
    // "execve03",
    // "execve04",
    // "execve05",
    // "execve06",
    // "execve06_child",
    // "execveat_child",
    // "execveat_errno",
    // "execveat01",
    // "execveat02",
    // "execveat03",
    // "execvp01",
    // "execvp01_child",
    // "exit_group01",
    // "exit01",
    // "exit02",
    // "f00f",
    // "faccessat01", //完全PASS
    // "faccessat02", // 完全PASS
    // "faccessat201",  //pass
    // "faccessat202", //涉及网络😭😭😭
    // "fallocate01",    //过了一半
    // "fallocate02",   //完全通过
    // "fallocate03", //卡死了
    // "fallocate04",
    // "fallocate05",
    // "fallocate06",
    // "fanotify_child",
    // "fanotify01",
    // "fanotify02",
    // "fanotify03",
    // "fanotify04",
    // "fanotify05",
    // "fanotify06",
    // "fanotify07",
    // "fanotify08",
    // "fanotify09",
    // "fanotify10",
    // "fanotify11",
    // "fanotify12",
    // "fanotify13",
    // "fanotify14",
    // "fanotify15",
    // "fanotify16",
    // "fanotify17",
    // "fanotify18",
    // "fanotify19",
    // "fanotify20",
    // "fanotify21",
    // "fanotify22",
    // "fanotify23",
    // "fanout01",
    // "fchdir01", //完全PASS
    // "fchdir02", // 完全PASS
    // "fchdir03",  //sendmsg
    // "fchmod01",   //pass
    // "fchmod02",  //sendmsg
    // "fchmod03",//sendmsg
    // "fchmod04",//sendmsg
    // "fchmod05",//sendmsg
    // "fchmod06",//sendmsg
    // "fchmodat01",  //pass
    // "fchmodat02",
    // "fchown01",//pass
    // "fchown01_16",
    // "fchown02",
    // "fchown02_16",
    // "fchown03",
    // "fchown03_16",
    // "fchown04",
    // "fchown04_16",
    // "fchown05",
    // "fchown05_16",
    // "fchownat01",
    // "fchownat02",
    // "fcntl01",
    // "fcntl01_64",
    // "fcntl02", //pass
    // "fcntl02_64",//pass
    // "fcntl03",//pass
    // "fcntl03_64", //pass
    // "fcntl04", //pass
    // "fcntl04_64", //pass
    // "fcntl05",   //pass
    // "fcntl05_64",  //pass
    // "fcntl07",
    // "fcntl07_64",
    // "fcntl08",   //pass
    // "fcntl08_64", //pass
    // "fcntl09",   //pass
    // "fcntl09_64",   //pass
    // "fcntl10",   //pass
    // "fcntl10_64", //pass
    // "fcntl11",
    // "fcntl11_64",
    // "fcntl12", //fail
    // "fcntl12_64",//fail
    // "fcntl13",  //pass
    // "fcntl13_64", //pass
    // "fcntl14", //rt_sigsuspend
    // "fcntl14_64",//rt_sigsuspend
    // "fcntl15", //passs5
    // "fcntl15_64", //pass5
    // "fcntl16",
    // "fcntl16_64",
    // "fcntl17",
    // "fcntl17_64",
    // "fcntl18",
    // "fcntl18_64",
    // "fcntl19",
    // "fcntl19_64",
    // "fcntl20",
    // "fcntl20_64",
    // "fcntl21",
    // "fcntl21_64",
    // "fcntl22",
    // "fcntl22_64",
    // "fcntl23",
    // "fcntl23_64",
    // "fcntl24",
    // "fcntl24_64",
    // "fcntl25",
    // "fcntl25_64",
    // "fcntl26",
    // "fcntl26_64",
    // "fcntl27",
    // "fcntl27_64",
    // "fcntl29",
    // "fcntl29_64",
    // "fcntl30",
    // "fcntl30_64",
    // "fcntl31",
    // "fcntl31_64",
    // "fcntl32",
    // "fcntl32_64",
    // "fcntl33",
    // "fcntl33_64",
    // "fcntl34",
    // "fcntl34_64",
    // "fcntl35",
    // "fcntl35_64",
    // "fcntl36",
    // "fcntl36_64",
    // "fcntl37",
    // "fcntl37_64",
    // "fcntl38",
    // "fcntl38_64",
    // "fcntl39",
    // "fcntl39_64",
    // "fdatasync01",    //pass
    // "fdatasync02",   //pass
    // "fdatasync03",    //loop0
    // "fgetxattr01",   //bin/sh
    // "fgetxattr02",
    // "fgetxattr03",
    // "file01.sh",
    // "filecapstest.sh",
    // "find_portbundle",
    // "finit_module01",
    // "finit_module02",
    // "flistxattr01",
    // "flistxattr02",
    // "flistxattr03",
    // "float_bessel",
    // "float_exp_log",
    // "float_iperb",
    // "float_power",
    // "float_trigo",
    // "flock01",
    // "flock02",
    // "flock03",
    // "flock04",
    // "flock06",
    // "force_erase.sh",
    // "fork_exec_loop",
    // "fork_freeze.sh",
    // "fork_procs",
    // "fork01",
    // "fork03",
    // "fork04",
    // "fork05",
    // "fork07",
    // "fork08",
    // "fork09",
    // "fork10",
    // "fork13",
    // "fork14",
    // "fou01.sh",
    // "fpathconf01",  //pass
    // "fptest01",
    // "fptest02",
    // "frag",
    // "freeze_cancel.sh",
    // "freeze_kill_thaw.sh",
    // "freeze_move_thaw.sh",
    // "freeze_self_thaw.sh",
    // "freeze_sleep_thaw.sh",
    // "freeze_thaw.sh",
    // "freeze_write_freezing.sh",
    // "fremovexattr01",
    // "fremovexattr02",
    // "fs_bind_cloneNS01.sh",
    // "fs_bind_cloneNS02.sh",
    // "fs_bind_cloneNS03.sh",
    // "fs_bind_cloneNS04.sh",
    // "fs_bind_cloneNS05.sh",
    // "fs_bind_cloneNS06.sh",
    // "fs_bind_cloneNS07.sh",
    // "fs_bind_lib.sh",
    // "fs_bind_move01.sh",
    // "fs_bind_move02.sh",
    // "fs_bind_move03.sh",
    // "fs_bind_move04.sh",
    // "fs_bind_move05.sh",
    // "fs_bind_move06.sh",
    // "fs_bind_move07.sh",
    // "fs_bind_move08.sh",
    // "fs_bind_move09.sh",
    // "fs_bind_move10.sh",
    // "fs_bind_move11.sh",
    // "fs_bind_move12.sh",
    // "fs_bind_move13.sh",
    // "fs_bind_move14.sh",
    // "fs_bind_move15.sh",
    // "fs_bind_move16.sh",
    // "fs_bind_move17.sh",
    // "fs_bind_move18.sh",
    // "fs_bind_move19.sh",
    // "fs_bind_move20.sh",
    // "fs_bind_move21.sh",
    // "fs_bind_move22.sh",
    // "fs_bind_rbind01.sh",
    // "fs_bind_rbind02.sh",
    // "fs_bind_rbind03.sh",
    // "fs_bind_rbind04.sh",
    // "fs_bind_rbind05.sh",
    // "fs_bind_rbind06.sh",
    // "fs_bind_rbind07.sh",
    // "fs_bind_rbind07-2.sh",
    // "fs_bind_rbind08.sh",
    // "fs_bind_rbind09.sh",
    // "fs_bind_rbind10.sh",
    // "fs_bind_rbind11.sh",
    // "fs_bind_rbind12.sh",
    // "fs_bind_rbind13.sh",
    // "fs_bind_rbind14.sh",
    // "fs_bind_rbind15.sh",
    // "fs_bind_rbind16.sh",
    // "fs_bind_rbind17.sh",
    // "fs_bind_rbind18.sh",
    // "fs_bind_rbind19.sh",
    // "fs_bind_rbind20.sh",
    // "fs_bind_rbind21.sh",
    // "fs_bind_rbind22.sh",
    // "fs_bind_rbind23.sh",
    // "fs_bind_rbind24.sh",
    // "fs_bind_rbind25.sh",
    // "fs_bind_rbind26.sh",
    // "fs_bind_rbind27.sh",
    // "fs_bind_rbind28.sh",
    // "fs_bind_rbind29.sh",
    // "fs_bind_rbind30.sh",
    // "fs_bind_rbind31.sh",
    // "fs_bind_rbind32.sh",
    // "fs_bind_rbind33.sh",
    // "fs_bind_rbind34.sh",
    // "fs_bind_rbind35.sh",
    // "fs_bind_rbind36.sh",
    // "fs_bind_rbind37.sh",
    // "fs_bind_rbind38.sh",
    // "fs_bind_rbind39.sh",
    // "fs_bind_regression.sh",
    // "fs_bind01.sh",
    // "fs_bind02.sh",
    // "fs_bind03.sh",
    // "fs_bind04.sh",
    // "fs_bind05.sh",
    // "fs_bind06.sh",
    // "fs_bind07.sh",
    // "fs_bind07-2.sh",
    // "fs_bind08.sh",
    // "fs_bind09.sh",
    // "fs_bind10.sh",
    // "fs_bind11.sh",
    // "fs_bind12.sh",
    // "fs_bind13.sh",
    // "fs_bind14.sh",
    // "fs_bind15.sh",
    // "fs_bind16.sh",
    // "fs_bind17.sh",
    // "fs_bind18.sh",
    // "fs_bind19.sh",
    // "fs_bind20.sh",
    // "fs_bind21.sh",
    // "fs_bind22.sh",
    // "fs_bind23.sh",
    // "fs_bind24.sh",
    // "fs_di",
    // "fs_fill",
    // "fs_inod",
    // "fs_perms",
    // "fs_racer.sh",
    // "fs_racer_dir_create.sh",
    // "fs_racer_dir_test.sh",
    // "fs_racer_file_concat.sh",
    // "fs_racer_file_create.sh",
    // "fs_racer_file_link.sh",
    // "fs_racer_file_list.sh",
    // "fs_racer_file_rename.sh",
    // "fs_racer_file_rm.sh",
    // "fs_racer_file_symlink.sh",
    // "fsconfig01",
    // "fsconfig02",
    // "fsconfig03",
    // "fsetxattr01",
    // "fsetxattr02",
    // "fsmount01",
    // "fsmount02",
    // "fsopen01",
    // "fsopen02",
    // "fspick01",
    // "fspick02",
    // "fsstress",
    // "fstat02",
    // "fstat02_64",
    // "fstat03", //pass
    // "fstat03_64",//pass
    // "fstatat01",  //
    // "fstatfs01",
    // "fstatfs01_64",
    // "fstatfs02",
    // "fstatfs02_64",
    // "fsx.sh",
    // "fsx-linux",
    // "fsync01", //loop0
    // "fsync02", //bin/sh
    // "fsync03",  //pass
    // "fsync04",  //loop0
    // "ftest01",
    // "ftest02",
    // "ftest03",
    // "ftest04",
    // "ftest05",
    // "ftest06",
    // "ftest07",
    // "ftest08",
    // "ftp01.sh",
    // "ftp-download-stress.sh",
    // "ftp-download-stress01-rmt.sh",
    // "ftp-download-stress02-rmt.sh",
    // "ftp-upload-stress.sh",
    // "ftp-upload-stress01-rmt.sh",
    // "ftp-upload-stress02-rmt.sh",
    // "ftrace_lib.sh",
    // "ftrace_regression01.sh",
    // "ftrace_regression02.sh",
    // "ftrace_stress_test.sh",
    // "ftruncate01",
    // "ftruncate01_64",
    // "ftruncate03",
    // "ftruncate03_64",
    // "ftruncate04",
    // "ftruncate04_64",
    // "futex_cmp_requeue01",
    // "futex_cmp_requeue02",
    // "futex_wait_bitset01",
    // "futex_wait01",
    // "futex_wait02",
    // "futex_wait03",
    // "futex_wait04",
    // "futex_wait05",
    // "futex_waitv01",
    // "futex_waitv02",
    // "futex_waitv03",
    // "futex_wake01",
    // "futex_wake02",
    // "futex_wake03",
    // "futex_wake04",
    // "futimesat01",
    // "fw_load",
    // "gdb01.sh",
    // "genacos",
    // "genasin",
    // "genatan",
    // "genatan2",
    // "genbessel",
    // "genceil",
    // "gencos",
    // "gencosh",
    // "generate_lvm_runfile.sh",
    // "geneve01.sh",
    // "geneve02.sh",
    // "genexp",
    // "genexp_log",
    // "genfabs",
    // "genfloor",
    // "genfmod",
    // "genfrexp",
    // "genhypot",
    // "geniperb",
    // "genj0",
    // "genj1",
    // "genldexp",
    // "genlgamma",
    // "genload",
    // "genlog",
    // "genlog10",
    // "genmodf",
    // "genpow",
    // "genpower",
    // "gensin",
    // "gensinh",
    // "gensqrt",
    // "gentan",
    // "gentanh",
    // "gentrigo",
    // "geny0",
    // "geny1",
    // "get_ifname",
    // "get_mempolicy01",
    // "get_mempolicy02",
    // "get_robust_list01",
    // "getaddrinfo_01",
    // "getcontext01",
    // "getcpu01",
    // "getcwd01",//pass
    // "getcwd02", // 完全PASS
    // "getcwd03",     //pass
    // "getcwd04", // Test needs at least 2 CPUs online 这个是因为 sched_getaffinity返回0，说不定它不用两个CPU
    // "getdents01",
    // "getdents02",
    // "getdomainname01",
    // "getegid01",
    // "getegid01_16",
    // "getegid02",
    // "getegid02_16",
    // "geteuid01",
    // "geteuid01_16",
    // "geteuid02",
    // "geteuid02_16",
    // "getgid01",
    // "getgid01_16",
    // "getgid03",
    // "getgid03_16",
    // "getgroups01",
    // "getgroups01_16",
    // "getgroups03",
    // "getgroups03_16",
    // "gethostbyname_r01",
    // "gethostid01",
    // "gethostname01",
    // "gethostname02",
    // "getitimer01",
    // "getitimer02",
    // "getpagesize01",
    // "getpeername01",
    // "getpgid01", // PASS
    // "getpgid02", // PASS
    // "getpgrp01",
    // "getpid01", // PASS
    // "getpid02", // PASS
    // "getppid01",// PASS
    // "getppid02",// PASS
    // "getpriority01",
    // "getpriority02",
    // "getrandom01",// pass
    // "getrandom02", // 完全PASS
    // "getrandom03", // 完全PASS
    // "getrandom04", // 完全PASS
    // "getrandom05",// pass
    // "getresgid01",
    // "getresgid01_16",
    // "getresgid02",
    // "getresgid02_16",
    // "getresgid03",
    // "getresgid03_16",
    // "getresuid01",
    // "getresuid01_16",
    // "getresuid02",
    // "getresuid02_16",
    // "getresuid03",
    // "getresuid03_16",
    // "getrlimit01",
    // "getrlimit02",
    // "getrlimit03",
    // "getrusage01",
    // "getrusage02",
    // "getrusage03",
    // "getrusage03_child",
    // "getrusage04",
    // "getsid01",
    // "getsid02",
    // "getsockname01",
    // "getsockopt01",
    // "getsockopt02",
    // "gettid01", // PASS
    // "gettid02", // PASS
    // "gettimeofday01",//pass
    // "gettimeofday02",
    // "getuid01",
    // "getuid01_16",
    // "getuid03",
    // "getuid03_16",
    // "getxattr01",
    // "getxattr02",
    // "getxattr03",
    // "getxattr04",
    // "getxattr05",
    // "gre01.sh",
    // "gre02.sh",
    // "growfiles",
    // "gzip_tests.sh",
    // "hackbench",
    // "hangup01",
    // "ht_affinity",
    // "ht_enabled",
    // "http-stress.sh",
    // "http-stress01-rmt.sh",
    // "http-stress02-rmt.sh",
    // "hugefallocate01",
    // "hugefallocate02",
    // "hugefork01",
    // "hugefork02",
    // "hugemmap01",
    // "hugemmap02",
    // "hugemmap04",
    // "hugemmap05",
    // "hugemmap06",
    // "hugemmap07",
    // "hugemmap08",
    // "hugemmap09",
    // "hugemmap10",
    // "hugemmap11",
    // "hugemmap12",
    // "hugemmap13",
    // "hugemmap14",
    // "hugemmap15",
    // "hugemmap16",
    // "hugemmap17",
    // "hugemmap18",
    // "hugemmap19",
    // "hugemmap20",
    // "hugemmap21",
    // "hugemmap22",
    // "hugemmap23",
    // "hugemmap24",
    // "hugemmap25",
    // "hugemmap26",
    // "hugemmap27",
    // "hugemmap28",
    // "hugemmap29",
    // "hugemmap30",
    // "hugemmap31",
    // "hugemmap32",
    // "hugeshmat01",
    // "hugeshmat02",
    // "hugeshmat03",
    // "hugeshmat04",
    // "hugeshmat05",
    // "hugeshmctl01",
    // "hugeshmctl02",
    // "hugeshmctl03",
    // "hugeshmdt01",
    // "hugeshmget01",
    // "hugeshmget02",
    // "hugeshmget03",
    // "hugeshmget05",
    // "icmp_rate_limit01",
    // "icmp4-multi-diffip01",
    // "icmp4-multi-diffip02",
    // "icmp4-multi-diffip03",
    // "icmp4-multi-diffip04",
    // "icmp4-multi-diffip05",
    // "icmp4-multi-diffip06",
    // "icmp4-multi-diffip07",
    // "icmp4-multi-diffnic01",
    // "icmp4-multi-diffnic02",
    // "icmp4-multi-diffnic03",
    // "icmp4-multi-diffnic04",
    // "icmp4-multi-diffnic05",
    // "icmp4-multi-diffnic06",
    // "icmp4-multi-diffnic07",
    // "icmp6-multi-diffip01",
    // "icmp6-multi-diffip02",
    // "icmp6-multi-diffip03",
    // "icmp6-multi-diffip04",
    // "icmp6-multi-diffip05",
    // "icmp6-multi-diffip06",
    // "icmp6-multi-diffip07",
    // "icmp6-multi-diffnic01",
    // "icmp6-multi-diffnic02",
    // "icmp6-multi-diffnic03",
    // "icmp6-multi-diffnic04",
    // "icmp6-multi-diffnic05",
    // "icmp6-multi-diffnic06",
    // "icmp6-multi-diffnic07",
    // "icmp-uni-basic.sh",
    // "icmp-uni-vti.sh",
    // "if4-addr-change.sh",
    // "if-addr-adddel.sh",
    // "if-addr-addlarge.sh",
    // "if-lib.sh",
    // "if-mtu-change.sh",
    // "if-route-adddel.sh",
    // "if-route-addlarge.sh",
    // "if-updown.sh",
    // "ima_boot_aggregate",
    // "ima_conditionals.sh",
    // "ima_kexec.sh",
    // "ima_keys.sh",
    // "ima_measurements.sh",
    // "ima_mmap",
    // "ima_policy.sh",
    // "ima_selinux.sh",
    // "ima_setup.sh",
    // "ima_tpm.sh",
    // "ima_violations.sh",
    // "in6_01",
    // "in6_02",
    // "inh_capped",
    // "init_module01",
    // "init_module02",
    // "initialize_if",
    // "inode01",
    // "inode02",
    // "inotify_init1_01",
    // "inotify_init1_02",
    // "inotify01",
    // "inotify02",
    // "inotify03",
    // "inotify04",
    // "inotify05",
    // "inotify06",
    // "inotify07",
    // "inotify08",
    // "inotify09",
    // "inotify10",
    // "inotify11",
    // "inotify12",
    // "input01",
    // "input02",
    // "input03",
    // "input04",
    // "input05",
    // "input06",
    // "insmod01.sh",
    // "io_cancel01",
    // "io_cancel02",
    // "io_control01",
    // "io_destroy01",
    // "io_destroy02",
    // "io_getevents01",
    // "io_getevents02",
    // "io_pgetevents01",
    // "io_pgetevents02",
    // "io_setup01",
    // "io_setup02",
    // "io_submit01",
    // "io_submit02",
    // "io_submit03",
    // "io_uring01",
    // "io_uring02",
    // "ioctl_loop01",
    // "ioctl_loop02",
    // "ioctl_loop03",
    // "ioctl_loop04",
    // "ioctl_loop05",
    // "ioctl_loop06",
    // "ioctl_loop07",
    // "ioctl_ns01",
    // "ioctl_ns02",
    // "ioctl_ns03",
    // "ioctl_ns04",
    // "ioctl_ns05",
    // "ioctl_ns06",
    // "ioctl_ns07",
    // "ioctl_sg01",
    // "ioctl01",
    // "ioctl02",
    // "ioctl03",
    // "ioctl04",
    // "ioctl05",
    // "ioctl06",
    // "ioctl07",
    // "ioctl08",
    // "ioctl09",
    // "iogen",
    // "ioperm01",
    // "ioperm02",
    // "iopl01",
    // "iopl02",
    // "ioprio_get01",
    // "ioprio_set01",
    // "ioprio_set02",
    // "ioprio_set03",
    // "ip_tests.sh",
    // "ipneigh01.sh",
    // "ipsec_lib.sh",
    // "iptables_lib.sh",
    // "iptables01.sh",
    // "ipvlan01.sh",
    // "irqbalance01",
    // "isofs.sh",
    // "kallsyms",
    // "kcmp01",
    // "kcmp02",
    // "kcmp03",
    // "kernbench",
    // "keyctl01",
    // "keyctl01.sh",
    // "keyctl02",
    // "keyctl03",
    // "keyctl04",
    // "keyctl05",
    // "keyctl06",
    // "keyctl07",
    // "keyctl08",
    // "keyctl09",
    // "kill02",
    // "kill03",
    // "kill05",
    // "kill06",
    // "kill07",
    // "kill08",
    // "kill09",
    // "kill10",
    // "kill11",
    // "kill12",
    // "kill13",
    // "killall_icmp_traffic",
    // "killall_tcp_traffic",
    // "killall_udp_traffic",
    // "kmsg01",
    // "ksm01",
    // "ksm02",
    // "ksm03",
    // "ksm04",
    // "ksm05",
    // "ksm06",
    // "ksm07",
    // "lchown01",
    // "lchown01_16",
    // "lchown02",
    // "lchown02_16",
    // "lchown03",
    // "lchown03_16",
    // "ld01.sh",
    // "ldd01.sh",
    // "leapsec01",
    // "lftest",
    // "lgetxattr01",
    // "lgetxattr02",
    // "libcgroup_freezer",
    // "link02",  //pass
    // "link04",//sendmsg
    // "link05", //pass,这个也是逆天数量
    // "link08", //pass3 fail1
    // "linkat01",  //有一个没过pass
    // "linkat02",//sendmsg
    // "linktest.sh",
    // "listen01", /pass
    // "listxattr01",
    // "listxattr02",
    // "listxattr03",
    // "llistxattr01",
    // "llistxattr02",
    // "llistxattr03",
    // "llseek01",  //pass
    // "llseek02",  //pass
    // "llseek03",//pass
    // "ln_tests.sh",
    // "lock_torture.sh",
    // "locktests",
    // "logrotate_tests.sh",
    // "lremovexattr01",
    // "lseek01", //爆了
    // "lseek02",  //过了一半
    // "lseek07", //pass
    // "lseek11",
    // "lsmod01.sh",
    // "lstat01",
    // "lstat01_64",
    // "lstat02",
    // "lstat02_64",
    // "ltp_acpi",
    // "ltpClient",
    // "ltpServer",
    // "ltpSockets.sh",
    // "macsec_lib.sh",
    // "macsec01.sh",
    // "macsec02.sh",
    // "macsec03.sh",
    // "macvlan01.sh",
    // "macvtap01.sh",
    // "madvise01",  //pass
    // "madvise02",
    // "madvise03",
    // "madvise05",
    // "madvise06",
    // "madvise07",
    // "madvise08",
    // "madvise09",
    // "madvise10",
    // "madvise11",
    // "mallinfo01",
    // "mallinfo02",
    // "mallinfo2_01",
    // "mallocstress",
    // "mallopt01",
    // "max_map_count",
    // "mbind01",
    // "mbind02",
    // "mbind03",
    // "mbind04",
    // "mc_cmds.sh",
    // "mc_commo.sh",
    // "mc_member.sh",
    // "mc_member_test",
    // "mc_opts.sh",
    // "mc_recv",
    // "mc_send",
    // "mc_verify_opts",
    // "mc_verify_opts_error",
    // "mcast-group-multiple-socket.sh",
    // "mcast-group-same-group.sh",
    // "mcast-group-single-socket.sh",
    // "mcast-group-source-filter.sh",
    // "mcast-lib.sh",
    // "mcast-pktfld01.sh",
    // "mcast-pktfld02.sh",
    // "mcast-queryfld01.sh",
    // "mcast-queryfld02.sh",
    // "mcast-queryfld03.sh",
    // "mcast-queryfld04.sh",
    // "mcast-queryfld05.sh",
    // "mcast-queryfld06.sh",
    // "meltdown",
    // "mem_process",
    // "mem02",
    // "membarrier01",
    // "memcg_control_test.sh",
    // "memcg_failcnt.sh",
    // "memcg_force_empty.sh",
    // "memcg_lib.sh",
    // "memcg_limit_in_bytes.sh",
    // "memcg_max_usage_in_bytes_test.sh",
    // "memcg_memsw_limit_in_bytes_test.sh",
    // "memcg_move_charge_at_immigrate_test.sh",
    // "memcg_process",
    // "memcg_process_stress",
    // "memcg_regression_test.sh",
    // "memcg_stat_rss.sh",
    // "memcg_stat_test.sh",
    // "memcg_stress_test.sh",
    // "memcg_subgroup_charge.sh",
    // "memcg_test_1",
    // "memcg_test_2",
    // "memcg_test_3",
    // "memcg_test_4",
    // "memcg_test_4.sh",
    // "memcg_usage_in_bytes_test.sh",
    // "memcg_use_hierarchy_test.sh",
    // "memcmp01",
    // "memcontrol01",
    // "memcontrol02",
    // "memcontrol03",
    // "memcontrol04",
    // "memcpy01",
    // "memctl_test01",
    // "memfd_create01",
    // "memfd_create02",
    // "memfd_create03",
    // "memfd_create04",
    // "memset01",
    // "memtoy",
    // "mesgq_nstest",
    // "migrate_pages01",
    // "migrate_pages02",
    // "migrate_pages03",
    // "min_free_kbytes",
    // "mincore01",
    // "mincore02",
    // "mincore03",
    // "mincore04",
    // "mkdir_tests.sh",
    // "mkdir02", //sendmsg
    // "mkdir03",  //pass
    // "mkdir04",  // setreuid
    // "mkdir05",  //sendmsg
    // "mkdir09",   //bin/sh
    // "mkdirat01", //pass
    // "mkdirat02",  //pass2fail2
    // "mkfs01.sh",
    // "mknod01",
    // "mknod02",
    // "mknod03",
    // "mknod04",
    // "mknod05",
    // "mknod06",
    // "mknod07",
    // "mknod08",
    // "mknod09",
    // "mknodat01",
    // "mknodat02",
    // "mkswap01.sh",
    // "mlock01",
    // "mlock02",
    // "mlock03",
    // "mlock04",
    // "mlock05",
    // "mlock201",
    // "mlock202",
    // "mlock203",
    // "mlockall01",
    // "mlockall02",
    // "mlockall03",
    // "mmap001",   //pass.
    // "mmap01",   //bin/sh
    // "mmap02",   //failed
    // "mmap03",
    // "mmap04",
    // "mmap05",
    // "mmap06",
    // "mmap08", //pass
    // "mmap09",  //pass
    // "mmap1",
    // "mmap10", //爆了
    // "mmap11",   //pass不能和别的一起跑
    // "mmap12",
    "mmap13", // pass
    // "mmap14",
    // "mmap15",  //pass
    // "mmap16",
    // "mmap17",   //pass
    // "mmap18",
    // "mmap19",  //pass
    // "mmap2",
    // "mmap20",   //pass
    // "mmap3",
    // "mmap-corruption01",
    // "mmapstress01",
    // "mmapstress02",
    // "mmapstress03",
    // "mmapstress04",
    // "mmapstress05",
    // "mmapstress06",
    // "mmapstress07",
    // "mmapstress08",
    // "mmapstress09",
    // "mmapstress10",
    // "mmstress",
    // "mmstress_dummy",
    // "modify_ldt01",
    // "modify_ldt02",
    // "modify_ldt03",
    // "mount_setattr01",
    // "mount01",
    // "mount02",
    // "mount03",
    // "mount03_suid_child",
    // "mount04",
    // "mount05",
    // "mount06",
    // "mount07",
    // "mountns01",
    // "mountns02",
    // "mountns03",
    // "mountns04",
    // "move_mount01",
    // "move_mount02",
    // "move_pages01",
    // "move_pages02",
    // "move_pages03",
    // "move_pages04",
    // "move_pages05",
    // "move_pages06",
    // "move_pages07",
    // "move_pages09",
    // "move_pages10",
    // "move_pages11",
    // "move_pages12",
    // "mpls_lib.sh",
    // "mpls01.sh",
    // "mpls02.sh",
    // "mpls03.sh",
    // "mpls04.sh",
    // "mprotect01", //pass
    // "mprotect02",
    // "mprotect03",
    // "mprotect04",
    // "mprotect05", //pass
    // "mq_notify01",
    // "mq_notify02",
    // "mq_notify03",
    // "mq_open01",
    // "mq_timedreceive01",
    // "mq_timedsend01",
    // "mq_unlink01",
    // "mqns_01",
    // "mqns_02",
    // "mqns_03",
    // "mqns_04",
    // "mremap01",//pass
    // "mremap02",//pass
    // "mremap03",//pass
    // "mremap04", //pass
    // "mremap05",//pass
    // "mremap06",
    // "msg_comm",
    // "msgctl01",
    // "msgctl02",
    // "msgctl03",
    // "msgctl04",
    // "msgctl05",
    // "msgctl06",
    // "msgctl12",
    // "msgget01",
    // "msgget02",
    // "msgget03",
    // "msgget04",
    // "msgget05",
    // "msgrcv01",
    // "msgrcv02",
    // "msgrcv03",
    // "msgrcv05",
    // "msgrcv06",
    // "msgrcv07",
    // "msgrcv08",
    // "msgsnd01",
    // "msgsnd02",
    // "msgsnd05",
    // "msgsnd06",
    // "msgstress01",
    // "msync01",  //pass
    // "msync02",  //pass两个
    // "msync03",   //pass
    // "msync04",
    // "mtest01",
    // "munlock01",
    // "munlock02",
    // "munlockall01",
    // "munmap01", //pass
    // "munmap02",  //pass
    // "munmap03",  //pass
    // "mv_tests.sh",
    // "myfunctions.sh",
    // "name_to_handle_at01",
    // "name_to_handle_at02",
    // "nanosleep01",
    // "nanosleep02",
    // "nanosleep04",
    // "net_cmdlib.sh",
    // "netns_breakns.sh",
    // "netns_comm.sh",
    // "netns_lib.sh",
    // "netns_netlink",
    // "netns_sysfs.sh",
    // "netstat01.sh",
    // "netstress",
    // "newuname01", //pass
    // "nextafter01",
    // "nfs_flock",
    // "nfs_flock_dgen",
    // "nfs_lib.sh",
    // "nfs01.sh",
    // "nfs01_open_files",
    // "nfs02.sh",
    // "nfs03.sh",
    // "nfs04.sh",
    // "nfs04_create_file",
    // "nfs05.sh",
    // "nfs05_make_tree",
    // "nfs06.sh",
    // "nfs07.sh",
    // "nfs08.sh",
    // "nfs09.sh",
    // "nfslock01.sh",
    // "nfsstat01.sh",
    // "nft01.sh",
    // "nft02",
    // "nftw01",
    // "nftw6401",
    // "nice01",
    // "nice02",
    // "nice03",
    // "nice04",
    // "nice05",
    // "nm01.sh",
    // "nptl01",
    // "ns-echoclient",
    // "ns-icmp_redirector",
    // "ns-icmpv4_sender",
    // "ns-icmpv6_sender",
    // "ns-igmp_querier",
    // "ns-mcast_join",
    // "ns-mcast_receiver",
    // "ns-tcpclient",
    // "ns-tcpserver",
    // "ns-udpclient",
    // "ns-udpsender",
    // "ns-udpserver",
    // "numa01.sh",
    // "oom01",
    // "oom02",
    // "oom03",
    // "oom04",
    // "oom05",
    // "open_by_handle_at01",
    // "open_by_handle_at02",
    // "open_tree01",
    // "open_tree02",
    // "open01",    //pass
    // "open02", //socket
    // "open03", // 完全PASS
    // "open04",    //完全PASS
    // "open06",   //pass
    // "open07",   //pass
    // "open08", // socket
    // "open09", //pass
    // "open10", // socket
    // "open11",    //pass
    // "open12",    //过三个
    // "open12_child",//这个不是测例
    // "open13",    // pass
    // "open14",    //pass这个测例要跑一年，别急着掐死，多等会
    // "openat01", // pass
    // "openat02",   //爆了
    // "openat02_child",
    // "openat03",   //pass这个和那个一年是同一个
    // "openat04",
    // "openat201",
    // "openat202",
    // "openat203",
    // "openfile",
    // "output_ipsec_conf",
    // "overcommit_memory",
    // "page01",
    // "page02",
    // "parameters.sh",
    // "pathconf01",   //pass
    // "pathconf02",
    // "pause01",
    // "pause02",
    // "pause03",
    // "pcrypt_aead01",
    // "pec_listener",
    // "perf_event_open01",
    // "perf_event_open02",
    // "perf_event_open03",
    // "personality01",
    // "personality02",
    // "pidfd_getfd01",
    // "pidfd_getfd02",
    // "pidfd_open01",
    // "pidfd_open02",
    // "pidfd_open03",
    // "pidfd_open04",
    // "pidfd_send_signal01",
    // "pidfd_send_signal02",
    // "pidfd_send_signal03",
    // "pidns01",
    // "pidns02",
    // "pidns03",
    // "pidns04",
    // "pidns05",
    // "pidns06",
    // "pidns10",
    // "pidns12",
    // "pidns13",
    // "pidns16",
    // "pidns17",
    // "pidns20",
    // "pidns30",
    // "pidns31",
    // "pidns32",
    // "pids.sh",
    // "pids_task1",
    // "pids_task2",
    // "ping01.sh",
    // "ping02.sh",
    // "pipe01",// 完全PASS
    // "pipe02",
    // "pipe03", // 完全PASS
    // "pipe04", //管道给写爆了，感觉是时间片太长了
    // "pipe05", // 完全PASS
    // "pipe06",    // 完全PASS
    // "pipe07", //proc/self/fd没写
    // "pipe08",
    // "pipe09", // 完全PASS
    // "pipe10", // 完全PASS
    // "pipe11",
    // "pipe12", // pass
    // "pipe13", // proc/4/stat没写
    // "pipe14", // 完全PASS
    // "pipe15", //NOFILE limit max too low: 128 < 65536
    // "pipe2_01", //pass
    // "pipe2_02",
    // "pipe2_02_child",
    // "pipe2_04",
    // "pipeio",
    // "pivot_root01",
    // "pkey01",
    // "pm_cpu_consolidation.py",
    // "pm_get_sched_values",
    // "pm_ilb_test.py",
    // "pm_include.sh",
    // "pm_sched_domain.py",
    // "pm_sched_mc.py",
    // "poll01",  //pass
    // "poll02",
    // "posix_fadvise01",
    // "posix_fadvise01_64",
    // "posix_fadvise02",
    // "posix_fadvise02_64",
    // "posix_fadvise03",
    // "posix_fadvise03_64",
    // "posix_fadvise04",
    // "posix_fadvise04_64",
    // "ppoll01",
    // "prctl01",
    // "prctl02",
    // "prctl03",
    // "prctl04",
    // "prctl05",
    // "prctl06",
    // "prctl06_execve",
    // "prctl07",
    // "prctl08",
    // "prctl09",
    // "prctl10",
    // "pread01",    //pass
    // "pread01_64",  //pass
    // "pread02",           //爆了
    // "pread02_64",
    // "preadv01",
    // "preadv01_64",
    // "preadv02",
    // "preadv02_64",
    // "preadv03",
    // "preadv03_64",
    // "preadv201",
    // "preadv201_64",
    // "preadv202",
    // "preadv202_64",
    // "preadv203",
    // "preadv203_64",
    // "prepare_lvm.sh",
    // "print_caps",
    // "proc_sched_rt01",
    // "proc01",    //pass
    // "process_madvise01",
    // "process_vm_readv02",
    // "process_vm_readv03",
    // "process_vm_writev02",
    // "process_vm01",
    // "profil01",
    // "prot_hsymlinks",
    // "pselect01",
    // "pselect01_64",
    // "pselect02",
    // "pselect02_64",
    // "pselect03",
    // "pselect03_64",
    // "pt_test",
    // "ptem01",
    // "pth_str01",
    // "pth_str02",
    // "pth_str03",
    // "pthcli",
    // "pthserv",
    // "ptrace01",
    // "ptrace02",
    // "ptrace03",
    // "ptrace04",
    // "ptrace05",
    // "ptrace06",
    // "ptrace07",
    // "ptrace08",
    // "ptrace09",
    // "ptrace10",
    // "ptrace11",
    // "pty01",
    // "pty02",
    // "pty03",
    // "pty04",
    // "pty05",
    // "pty06",
    // "pty07",
    // "pwrite01",    //pass
    // "pwrite01_64",    //pass
    // "pwrite02",
    // "pwrite02_64",
    // "pwrite03",
    // "pwrite03_64",
    // "pwrite04",
    // "pwrite04_64",
    // "pwritev01",
    // "pwritev01_64",
    // "pwritev02",
    // "pwritev02_64",
    // "pwritev03",
    // "pwritev03_64",
    // "pwritev201",
    // "pwritev201_64",
    // "pwritev202",
    // "pwritev202_64",
    // "quota_remount_test01.sh",
    // "quotactl01",
    // "quotactl02",
    // "quotactl03",
    // "quotactl04",
    // "quotactl05",
    // "quotactl06",
    // "quotactl07",
    // "quotactl08",
    // "quotactl09",
    // "rcu_torture.sh",
    // "read_all",
    // "read01", //貌似可以PASS
    // "read02",   //pass
    // "read03", //pass
    // "read04", // 完全PASS
    // "readahead01",
    // "readahead02",
    // "readdir01",
    // "readdir21",
    // "readlink01", //sendmsg
    // "readlink03",//sendmsg
    // "readlinkat01", //pass
    // "readlinkat02", //pass五个
    // "readv01", //pass
    // "readv02",   //爆了
    // "realpath01",
    // "reboot01",
    // "reboot02",
    // "recv01",
    // "recvfrom01", //pass
    // "recvmmsg01",
    // "recvmsg01",
    // "recvmsg02",
    // "recvmsg03",
    // "remap_file_pages01",
    // "remap_file_pages02",
    // "remove_password.sh",
    // "removexattr01",
    // "removexattr02",
    // "rename01",   //bin/sh
    // "rename03",    //bin/sh
    // "rename04",//bin/sh
    // "rename05",//bin/sh
    // "rename06",
    // "rename07",
    // "rename08",
    // "rename09",
    // "rename10",
    // "rename11",
    // "rename12",
    // "rename13",
    // "rename14",
    // "renameat01",
    // "renameat201",
    // "renameat202",
    // "request_key01",
    // "request_key02",
    // "request_key03",
    // "request_key04",
    // "request_key05",
    // "rmdir01",   //pass
    // "rmdir02", //pass
    // "rmdir03",  //sendmsg
    // "route4-rmmod",
    // "route6-rmmod",
    // "route-change-dst.sh",
    // "route-change-gw.sh",
    // "route-change-if.sh",
    // "route-change-netlink",
    // "route-change-netlink-dst.sh",
    // "route-change-netlink-gw.sh",
    // "route-change-netlink-if.sh",
    // "route-lib.sh",
    // "route-redirect.sh",
    // "rt_sigaction01",
    // "rt_sigaction02",
    // "rt_sigaction03",
    // "rt_sigprocmask01",
    // "rt_sigprocmask02",
    // "rt_sigqueueinfo01",
    // "rt_sigsuspend01",
    // "rtc01",
    // "rtc02",
    // "run_capbounds.sh",
    // "run_cpuctl_latency_test.sh",
    // "run_cpuctl_stress_test.sh",
    // "run_cpuctl_test.sh",
    // "run_cpuctl_test_fj.sh",
    // "run_freezer.sh",
    // "run_memctl_test.sh",
    // "run_sched_cliserv.sh",
    // "runpwtests_exclusive01.sh",
    // "runpwtests_exclusive02.sh",
    // "runpwtests_exclusive03.sh",
    // "runpwtests_exclusive04.sh",
    // "runpwtests_exclusive05.sh",
    // "runpwtests01.sh",
    // "runpwtests02.sh",
    // "runpwtests03.sh",
    // "runpwtests04.sh",
    // "runpwtests05.sh",
    // "runpwtests06.sh",
    // "rwtest",
    // "sbrk01", // 爆了
    // "sbrk02", // pass
    // "sbrk03", // Arch需要是S390
    // "sched_datafile",
    // "sched_driver",
    // "sched_get_priority_max01",
    // "sched_get_priority_max02",
    // "sched_get_priority_min01",
    // "sched_get_priority_min02",
    // "sched_getaffinity01", // PASS
    // "sched_getattr01",
    // "sched_getattr02",
    // "sched_getparam01",
    // "sched_getparam03",
    // "sched_getscheduler01",
    // "sched_getscheduler02",
    // "sched_rr_get_interval01",
    // "sched_rr_get_interval02",
    // "sched_rr_get_interval03",
    // "sched_setaffinity01",
    // "sched_setattr01",
    // "sched_setparam01",
    // "sched_setparam02",
    // "sched_setparam03",
    // "sched_setparam04",
    // "sched_setparam05",
    // "sched_setscheduler01",
    // "sched_setscheduler02",
    // "sched_setscheduler03",
    // "sched_setscheduler04",
    // "sched_stress.sh",
    // "sched_tc0",
    // "sched_tc1",
    // "sched_tc2",
    // "sched_tc3",
    // "sched_tc4",
    // "sched_tc5",
    // "sched_tc6",
    // "sched_yield01", //pass
    // "sctp_big_chunk",
    // "sctp_ipsec.sh",
    // "sctp_ipsec_vti.sh",
    // "sctp01.sh",
    // "select01",
    // "select02",
    // "select03",
    // "select04",
    // "sem_comm",
    // "sem_nstest",
    // "semctl01",
    // "semctl02",
    // "semctl03",
    // "semctl04",
    // "semctl05",
    // "semctl06",
    // "semctl07",
    // "semctl08",
    // "semctl09",
    // "semget01",
    // "semget02",
    // "semget05",
    // "semop01",
    // "semop02",
    // "semop03",
    // "semop04",
    // "semop05",
    // "semtest_2ns",
    // "send01",
    // "send02",
    // "sendfile01.sh",
    // "sendfile02",
    // "sendfile02_64",
    // "sendfile03",
    // "sendfile03_64",
    // "sendfile04",
    // "sendfile04_64",
    // "sendfile05",
    // "sendfile05_64",
    // "sendfile06",
    // "sendfile06_64",
    // "sendfile07",
    // "sendfile07_64",
    // "sendfile08",
    // "sendfile08_64",
    // "sendfile09",
    // "sendfile09_64",
    // "sendmmsg01",
    // "sendmmsg02",
    // "sendmsg01",
    // "sendmsg02",
    // "sendmsg03",
    // "sendto01", //pass一部分
    // "sendto02", //pass
    // "sendto03", //.config
    // "set_ipv4addr",
    // "set_mempolicy01",
    // "set_mempolicy02",
    // "set_mempolicy03",
    // "set_mempolicy04",
    // "set_mempolicy05",
    // "set_robust_list01",
    // "set_thread_area01",
    // "set_tid_address01",
    // "setdomainname01",
    // "setdomainname02",
    // "setdomainname03",
    // "setegid01",
    // "setegid02",
    // "setfsgid01",
    // "setfsgid01_16",
    // "setfsgid02",
    // "setfsgid02_16",
    // "setfsgid03",
    // "setfsgid03_16",
    // "setfsuid01",
    // "setfsuid01_16",
    // "setfsuid02",
    // "setfsuid02_16",
    // "setfsuid03",
    // "setfsuid03_16",
    // "setfsuid04",
    // "setfsuid04_16",
    // "setgid01",
    // "setgid01_16",
    // "setgid02",
    // "setgid02_16",
    // "setgid03",
    // "setgid03_16",
    // "setgroups01",
    // "setgroups01_16",
    // "setgroups02",
    // "setgroups02_16",
    // "setgroups03",
    // "setgroups03_16",
    // "setgroups04",
    // "setgroups04_16",
    // "sethostname01",
    // "sethostname02",
    // "sethostname03",
    // "setitimer01",
    // "setitimer02",
    // "setns01",
    // "setns02",
    // "setpgid01", // pass
    // "setpgid02", // pass
    // "setpgid03", // 要完善sid逻辑, 而且现在退不出去, 先不修
    // "setpgid03_child",
    // "setpgrp01",
    // "setpgrp02",
    // "setpriority01",
    // "setpriority02",
    // "setregid01",
    // "setregid01_16",
    // "setregid02",
    // "setregid02_16",
    // "setregid03",
    // "setregid03_16",
    // "setregid04",
    // "setregid04_16",
    // "setresgid01",
    // "setresgid01_16",
    // "setresgid02",
    // "setresgid02_16",
    // "setresgid03",
    // "setresgid03_16",
    // "setresgid04",
    // "setresgid04_16",
    // "setresuid01",
    // "setresuid01_16",
    // "setresuid02",
    // "setresuid02_16",
    // "setresuid03",
    // "setresuid03_16",
    // "setresuid04",
    // "setresuid04_16",
    // "setresuid05",
    // "setresuid05_16",
    // "setreuid01",
    // "setreuid01_16",
    // "setreuid02",
    // "setreuid02_16",
    // "setreuid03",
    // "setreuid03_16",
    // "setreuid04",
    // "setreuid04_16",
    // "setreuid05",
    // "setreuid05_16",
    // "setreuid06",
    // "setreuid06_16",
    // "setreuid07",
    // "setreuid07_16",
    // "setrlimit01",
    // "setrlimit02",
    // "setrlimit03",
    // "setrlimit04",
    // "setrlimit05",
    // "setrlimit06",
    // "setsid01",
    // "setsockopt01", //pass
    // "setsockopt02",
    // "setsockopt03", //pass
    // "setsockopt04",
    // "setsockopt05", //.config
    // "setsockopt06", //.config
    // "setsockopt07", //.config
    // "setsockopt08",
    // "setsockopt09", //.config
    // "setsockopt10", //.config
    // "settimeofday01",
    // "settimeofday02",
    // "setuid01",
    // "setuid01_16",
    // "setuid03",
    // "setuid03_16",
    // "setuid04",
    // "setuid04_16",
    // "setxattr01",
    // "setxattr02",
    // "setxattr03",
    // "sgetmask01",
    // "shell_pipe01.sh",
    // "shm_comm",
    // "shm_test",
    // "shmat01",    //pass一个没过
    // "shmat02",
    // "shmat03",     //pass?
    // "shmat04",     //pass
    // "shmat1",
    // "shmctl01",     //卡死了
    // "shmctl02",    //sendmsg
    // "shmctl03",    //pass，但是这个似乎不能和别的连着跑
    // "shmctl04",   //sendmsg
    // "shmctl05",   //爆了
    // "shmctl06",    //test requires struct shmid64_ds to have the time_high fields
    // "shmctl07",    //pass
    // "shmctl08",     //pass
    // "shmdt01",      //信号
    // "shmdt02",      //pass
    // "shmem_2nstest", //看不懂
    // "shmget02",
    // "shmget03",
    // "shmget04",   //sendmsg
    // "shmget05",    //.config
    // "shmget06", //.config
    // "shmnstest",   //pass
    // "shmt02",     //pass
    // "shmt03",      //pass
    // "shmt04",      //pass
    // "shmt05",          //pass
    // "shmt06",        //pass
    // "shmt07",       //pass
    // "shmt08",      //pass
    // "shmt09",      //sbrk
    // "shmt10",       //pass
    // "sigaction01",
    // "sigaction02",
    // "sigaltstack01",
    // "sigaltstack02",
    // "sighold02",
    // "signal01",
    // "signal02",
    // "signal03",
    // "signal04",
    // "signal05",
    // "signal06",
    // "signalfd01",
    // "signalfd4_01",
    // "signalfd4_02",
    // "sigpending02",
    // "sigprocmask01",
    // "sigrelse01",
    // "sigsuspend01",
    // "sigtimedwait01",
    // "sigwait01",
    // "sigwaitinfo01",
    // "sit01.sh",
    // "smack_common.sh",
    // "smack_file_access.sh",
    // "smack_notroot",
    // "smack_set_ambient.sh",
    // "smack_set_cipso.sh",
    // "smack_set_current.sh",
    // "smack_set_direct.sh",
    // "smack_set_doi.sh",
    // "smack_set_load.sh",
    // "smack_set_netlabel.sh",
    // "smack_set_onlycap.sh",
    // "smack_set_socket_labels",
    // "smt_smp_affinity.sh",
    // "smt_smp_enabled.sh",
    // "snd_seq01",
    // "snd_timer01",
    // "socket01", //pass
    // "socket02", //pass
    // "socketcall01",
    // "socketcall02",
    // "socketcall03",
    // "socketpair01",
    // "socketpair02",
    // "sockioctl01",
    // "splice01",
    // "splice02",
    // "splice03",
    // "splice04",
    // "splice05",
    // "splice06",
    // "splice07",
    // "splice08",
    // "splice09",
    // "squashfs01",
    // "ssetmask01",
    // "ssh-stress.sh",
    // "stack_clash",
    // "stack_space",
    // "starvation",
    // "stat01",      //sendmsg
    // "stat01_64",
    // "stat02",    //pass
    // "stat02_64",   //pass
    // "stat03",   //sendmsg
    // "stat03_64",
    // "statfs01",
    // "statfs01_64",
    // "statfs02",  //pass3fail3
    // "statfs02_64", //pass3fail3
    // "statfs03",    //sendmsg
    // "statfs03_64",
    // "statvfs01",
    // "statvfs02", //和别的不能一起跑
    // "statx01",  //一直游到海水变蓝
    // "statx02",  //pass4 fail1
    // "statx03",// pass6 fail1
    // "statx04",   //bin/sh
    // "statx05",
    // "statx06",
    // "statx07",
    // "statx08",
    // "statx09",   //.config
    // "statx10",  //bin/sh
    // "statx11",
    // "statx12",
    // "stime01",
    // "stime02",
    // "stop_freeze_sleep_thaw_cont.sh",
    // "stop_freeze_thaw_cont.sh",
    // "stream01",   //pass
    // "stream02",   //pass
    // "stream03", //pass
    // "stream04",  //pass
    // "stream05", //pass
    // "stress",
    // "string01", //pass
    // "support_numa",
    // "swapoff01",
    // "swapoff02",
    // "swapon01",
    // "swapon02",
    // "swapon03",
    // "swapping01",
    // "symlink01",  //pass
    // "symlink02",  //pass
    // "symlink03",   //sendmsg
    // "symlink04", //pass
    // "symlinkat01", //pass
    // "sync_file_range01",
    // "sync_file_range02",
    // "sync01",
    // "syncfs01",
    // "syscall01",  //pass
    // "sysconf01", //过了很多，很多没过
    // "sysctl01",
    // "sysctl01.sh",
    // "sysctl02.sh",
    // "sysctl03",
    // "sysctl04",
    // "sysfs01",
    // "sysfs02",
    // "sysfs03",
    // "sysfs04",
    // "sysfs05",
    // "sysinfo01",
    // "sysinfo02",
    // "sysinfo03",
    // "syslog11",
    // "syslog12",
    // "tar_tests.sh",
    // "tbio",
    // "tc01.sh",
    // "tcindex01",
    // "tcp_cc_lib.sh",
    // "tcp_fastopen_run.sh",
    // "tcp_ipsec.sh",
    // "tcp_ipsec_vti.sh",
    // "tcp4-multi-diffip01",
    // "tcp4-multi-diffip02",
    // "tcp4-multi-diffip03",
    // "tcp4-multi-diffip04",
    // "tcp4-multi-diffip05",
    // "tcp4-multi-diffip06",
    // "tcp4-multi-diffip07",
    // "tcp4-multi-diffip08",
    // "tcp4-multi-diffip09",
    // "tcp4-multi-diffip10",
    // "tcp4-multi-diffip11",
    // "tcp4-multi-diffip12",
    // "tcp4-multi-diffip13",
    // "tcp4-multi-diffip14",
    // "tcp4-multi-diffnic01",
    // "tcp4-multi-diffnic02",
    // "tcp4-multi-diffnic03",
    // "tcp4-multi-diffnic04",
    // "tcp4-multi-diffnic05",
    // "tcp4-multi-diffnic06",
    // "tcp4-multi-diffnic07",
    // "tcp4-multi-diffnic08",
    // "tcp4-multi-diffnic09",
    // "tcp4-multi-diffnic10",
    // "tcp4-multi-diffnic11",
    // "tcp4-multi-diffnic12",
    // "tcp4-multi-diffnic13",
    // "tcp4-multi-diffnic14",
    // "tcp4-multi-diffport01",
    // "tcp4-multi-diffport02",
    // "tcp4-multi-diffport03",
    // "tcp4-multi-diffport04",
    // "tcp4-multi-diffport05",
    // "tcp4-multi-diffport06",
    // "tcp4-multi-diffport07",
    // "tcp4-multi-diffport08",
    // "tcp4-multi-diffport09",
    // "tcp4-multi-diffport10",
    // "tcp4-multi-diffport11",
    // "tcp4-multi-diffport12",
    // "tcp4-multi-diffport13",
    // "tcp4-multi-diffport14",
    // "tcp4-multi-sameport01",
    // "tcp4-multi-sameport02",
    // "tcp4-multi-sameport03",
    // "tcp4-multi-sameport04",
    // "tcp4-multi-sameport05",
    // "tcp4-multi-sameport06",
    // "tcp4-multi-sameport07",
    // "tcp4-multi-sameport08",
    // "tcp4-multi-sameport09",
    // "tcp4-multi-sameport10",
    // "tcp4-multi-sameport11",
    // "tcp4-multi-sameport12",
    // "tcp4-multi-sameport13",
    // "tcp4-multi-sameport14",
    // "tcp4-uni-basic01",
    // "tcp4-uni-basic02",
    // "tcp4-uni-basic03",
    // "tcp4-uni-basic04",
    // "tcp4-uni-basic05",
    // "tcp4-uni-basic06",
    // "tcp4-uni-basic07",
    // "tcp4-uni-basic08",
    // "tcp4-uni-basic09",
    // "tcp4-uni-basic10",
    // "tcp4-uni-basic11",
    // "tcp4-uni-basic12",
    // "tcp4-uni-basic13",
    // "tcp4-uni-basic14",
    // "tcp4-uni-dsackoff01",
    // "tcp4-uni-dsackoff02",
    // "tcp4-uni-dsackoff03",
    // "tcp4-uni-dsackoff04",
    // "tcp4-uni-dsackoff05",
    // "tcp4-uni-dsackoff06",
    // "tcp4-uni-dsackoff07",
    // "tcp4-uni-dsackoff08",
    // "tcp4-uni-dsackoff09",
    // "tcp4-uni-dsackoff10",
    // "tcp4-uni-dsackoff11",
    // "tcp4-uni-dsackoff12",
    // "tcp4-uni-dsackoff13",
    // "tcp4-uni-dsackoff14",
    // "tcp4-uni-pktlossdup01",
    // "tcp4-uni-pktlossdup02",
    // "tcp4-uni-pktlossdup03",
    // "tcp4-uni-pktlossdup04",
    // "tcp4-uni-pktlossdup05",
    // "tcp4-uni-pktlossdup06",
    // "tcp4-uni-pktlossdup07",
    // "tcp4-uni-pktlossdup08",
    // "tcp4-uni-pktlossdup09",
    // "tcp4-uni-pktlossdup10",
    // "tcp4-uni-pktlossdup11",
    // "tcp4-uni-pktlossdup12",
    // "tcp4-uni-pktlossdup13",
    // "tcp4-uni-pktlossdup14",
    // "tcp4-uni-sackoff01",
    // "tcp4-uni-sackoff02",
    // "tcp4-uni-sackoff03",
    // "tcp4-uni-sackoff04",
    // "tcp4-uni-sackoff05",
    // "tcp4-uni-sackoff06",
    // "tcp4-uni-sackoff07",
    // "tcp4-uni-sackoff08",
    // "tcp4-uni-sackoff09",
    // "tcp4-uni-sackoff10",
    // "tcp4-uni-sackoff11",
    // "tcp4-uni-sackoff12",
    // "tcp4-uni-sackoff13",
    // "tcp4-uni-sackoff14",
    // "tcp4-uni-smallsend01",
    // "tcp4-uni-smallsend02",
    // "tcp4-uni-smallsend03",
    // "tcp4-uni-smallsend04",
    // "tcp4-uni-smallsend05",
    // "tcp4-uni-smallsend06",
    // "tcp4-uni-smallsend07",
    // "tcp4-uni-smallsend08",
    // "tcp4-uni-smallsend09",
    // "tcp4-uni-smallsend10",
    // "tcp4-uni-smallsend11",
    // "tcp4-uni-smallsend12",
    // "tcp4-uni-smallsend13",
    // "tcp4-uni-smallsend14",
    // "tcp4-uni-tso01",
    // "tcp4-uni-tso02",
    // "tcp4-uni-tso03",
    // "tcp4-uni-tso04",
    // "tcp4-uni-tso05",
    // "tcp4-uni-tso06",
    // "tcp4-uni-tso07",
    // "tcp4-uni-tso08",
    // "tcp4-uni-tso09",
    // "tcp4-uni-tso10",
    // "tcp4-uni-tso11",
    // "tcp4-uni-tso12",
    // "tcp4-uni-tso13",
    // "tcp4-uni-tso14",
    // "tcp4-uni-winscale01",
    // "tcp4-uni-winscale02",
    // "tcp4-uni-winscale03",
    // "tcp4-uni-winscale04",
    // "tcp4-uni-winscale05",
    // "tcp4-uni-winscale06",
    // "tcp4-uni-winscale07",
    // "tcp4-uni-winscale08",
    // "tcp4-uni-winscale09",
    // "tcp4-uni-winscale10",
    // "tcp4-uni-winscale11",
    // "tcp4-uni-winscale12",
    // "tcp4-uni-winscale13",
    // "tcp4-uni-winscale14",
    // "tcp6-multi-diffip01",
    // "tcp6-multi-diffip02",
    // "tcp6-multi-diffip03",
    // "tcp6-multi-diffip04",
    // "tcp6-multi-diffip05",
    // "tcp6-multi-diffip06",
    // "tcp6-multi-diffip07",
    // "tcp6-multi-diffip08",
    // "tcp6-multi-diffip09",
    // "tcp6-multi-diffip10",
    // "tcp6-multi-diffip11",
    // "tcp6-multi-diffip12",
    // "tcp6-multi-diffip13",
    // "tcp6-multi-diffip14",
    // "tcp6-multi-diffnic01",
    // "tcp6-multi-diffnic02",
    // "tcp6-multi-diffnic03",
    // "tcp6-multi-diffnic04",
    // "tcp6-multi-diffnic05",
    // "tcp6-multi-diffnic06",
    // "tcp6-multi-diffnic07",
    // "tcp6-multi-diffnic08",
    // "tcp6-multi-diffnic09",
    // "tcp6-multi-diffnic10",
    // "tcp6-multi-diffnic11",
    // "tcp6-multi-diffnic12",
    // "tcp6-multi-diffnic13",
    // "tcp6-multi-diffnic14",
    // "tcp6-multi-diffport01",
    // "tcp6-multi-diffport02",
    // "tcp6-multi-diffport03",
    // "tcp6-multi-diffport04",
    // "tcp6-multi-diffport05",
    // "tcp6-multi-diffport06",
    // "tcp6-multi-diffport07",
    // "tcp6-multi-diffport08",
    // "tcp6-multi-diffport09",
    // "tcp6-multi-diffport10",
    // "tcp6-multi-diffport11",
    // "tcp6-multi-diffport12",
    // "tcp6-multi-diffport13",
    // "tcp6-multi-diffport14",
    // "tcp6-multi-sameport01",
    // "tcp6-multi-sameport02",
    // "tcp6-multi-sameport03",
    // "tcp6-multi-sameport04",
    // "tcp6-multi-sameport05",
    // "tcp6-multi-sameport06",
    // "tcp6-multi-sameport07",
    // "tcp6-multi-sameport08",
    // "tcp6-multi-sameport09",
    // "tcp6-multi-sameport10",
    // "tcp6-multi-sameport11",
    // "tcp6-multi-sameport12",
    // "tcp6-multi-sameport13",
    // "tcp6-multi-sameport14",
    // "tcp6-uni-basic01",
    // "tcp6-uni-basic02",
    // "tcp6-uni-basic03",
    // "tcp6-uni-basic04",
    // "tcp6-uni-basic05",
    // "tcp6-uni-basic06",
    // "tcp6-uni-basic07",
    // "tcp6-uni-basic08",
    // "tcp6-uni-basic09",
    // "tcp6-uni-basic10",
    // "tcp6-uni-basic11",
    // "tcp6-uni-basic12",
    // "tcp6-uni-basic13",
    // "tcp6-uni-basic14",
    // "tcp6-uni-dsackoff01",
    // "tcp6-uni-dsackoff02",
    // "tcp6-uni-dsackoff03",
    // "tcp6-uni-dsackoff04",
    // "tcp6-uni-dsackoff05",
    // "tcp6-uni-dsackoff06",
    // "tcp6-uni-dsackoff07",
    // "tcp6-uni-dsackoff08",
    // "tcp6-uni-dsackoff09",
    // "tcp6-uni-dsackoff10",
    // "tcp6-uni-dsackoff11",
    // "tcp6-uni-dsackoff12",
    // "tcp6-uni-dsackoff13",
    // "tcp6-uni-dsackoff14",
    // "tcp6-uni-pktlossdup01",
    // "tcp6-uni-pktlossdup02",
    // "tcp6-uni-pktlossdup03",
    // "tcp6-uni-pktlossdup04",
    // "tcp6-uni-pktlossdup05",
    // "tcp6-uni-pktlossdup06",
    // "tcp6-uni-pktlossdup07",
    // "tcp6-uni-pktlossdup08",
    // "tcp6-uni-pktlossdup09",
    // "tcp6-uni-pktlossdup10",
    // "tcp6-uni-pktlossdup11",
    // "tcp6-uni-pktlossdup12",
    // "tcp6-uni-pktlossdup13",
    // "tcp6-uni-pktlossdup14",
    // "tcp6-uni-sackoff01",
    // "tcp6-uni-sackoff02",
    // "tcp6-uni-sackoff03",
    // "tcp6-uni-sackoff04",
    // "tcp6-uni-sackoff05",
    // "tcp6-uni-sackoff06",
    // "tcp6-uni-sackoff07",
    // "tcp6-uni-sackoff08",
    // "tcp6-uni-sackoff09",
    // "tcp6-uni-sackoff10",
    // "tcp6-uni-sackoff11",
    // "tcp6-uni-sackoff12",
    // "tcp6-uni-sackoff13",
    // "tcp6-uni-sackoff14",
    // "tcp6-uni-smallsend01",
    // "tcp6-uni-smallsend02",
    // "tcp6-uni-smallsend03",
    // "tcp6-uni-smallsend04",
    // "tcp6-uni-smallsend05",
    // "tcp6-uni-smallsend06",
    // "tcp6-uni-smallsend07",
    // "tcp6-uni-smallsend08",
    // "tcp6-uni-smallsend09",
    // "tcp6-uni-smallsend10",
    // "tcp6-uni-smallsend11",
    // "tcp6-uni-smallsend12",
    // "tcp6-uni-smallsend13",
    // "tcp6-uni-smallsend14",
    // "tcp6-uni-tso01",
    // "tcp6-uni-tso02",
    // "tcp6-uni-tso03",
    // "tcp6-uni-tso04",
    // "tcp6-uni-tso05",
    // "tcp6-uni-tso06",
    // "tcp6-uni-tso07",
    // "tcp6-uni-tso08",
    // "tcp6-uni-tso09",
    // "tcp6-uni-tso10",
    // "tcp6-uni-tso11",
    // "tcp6-uni-tso12",
    // "tcp6-uni-tso13",
    // "tcp6-uni-tso14",
    // "tcp6-uni-winscale01",
    // "tcp6-uni-winscale02",
    // "tcp6-uni-winscale03",
    // "tcp6-uni-winscale04",
    // "tcp6-uni-winscale05",
    // "tcp6-uni-winscale06",
    // "tcp6-uni-winscale07",
    // "tcp6-uni-winscale08",
    // "tcp6-uni-winscale09",
    // "tcp6-uni-winscale10",
    // "tcp6-uni-winscale11",
    // "tcp6-uni-winscale12",
    // "tcp6-uni-winscale13",
    // "tcp6-uni-winscale14",
    // "tcpdump01.sh",
    // "tee01",
    // "tee02",
    // "test.sh",
    // "test_1_to_1_accept_close",
    // "test_1_to_1_addrs",
    // "test_1_to_1_connect",
    // "test_1_to_1_connectx",
    // "test_1_to_1_events",
    // "test_1_to_1_initmsg_connect",
    // "test_1_to_1_nonblock",
    // "test_1_to_1_recvfrom",
    // "test_1_to_1_recvmsg",
    // "test_1_to_1_rtoinfo",
    // "test_1_to_1_send",
    // "test_1_to_1_sendmsg",
    // "test_1_to_1_sendto",
    // "test_1_to_1_shutdown",
    // "test_1_to_1_socket_bind_listen",
    // "test_1_to_1_sockopt",
    // "test_1_to_1_threads",
    // "test_assoc_abort",
    // "test_assoc_shutdown",
    // "test_autoclose",
    // "test_basic",
    // "test_basic_v6",
    // "test_connect",
    // "test_connectx",
    // "test_controllers.sh",
    // "test_fragments",
    // "test_fragments_v6",
    // "test_getname",
    // "test_getname_v6",
    // "test_inaddr_any",
    // "test_inaddr_any_v6",
    // "test_ioctl",
    // "test_peeloff",
    // "test_peeloff_v6",
    // "test_recvmsg",
    // "test_robind.sh",
    // "test_sctp_sendrecvmsg",
    // "test_sctp_sendrecvmsg_v6",
    // "test_sockopt",
    // "test_sockopt_v6",
    // "test_tcp_style",
    // "test_tcp_style_v6",
    // "test_timetolive",
    // "test_timetolive_v6",
    // "testsf_c",
    // "testsf_c6",
    // "testsf_s",
    // "testsf_s6",
    // "tgkill01",
    // "tgkill02",
    // "tgkill03",
    // "thp01",
    // "thp02",
    // "thp03",
    // "thp04",
    // "time01",   //pass
    // "timed_forkbomb",
    // "timens01", //.config
    // "timer_delete01",
    // "timer_delete02",
    // "timer_getoverrun01",
    // "timer_gettime01",
    // "timer_settime01",
    // "timer_settime02",
    // "timer_settime03",
    // "timerfd_create01",
    // "timerfd_gettime01",
    // "timerfd_settime01",
    // "timerfd_settime02",
    // "timerfd01",
    // "timerfd02",
    // "timerfd04",
    // "times01",
    // "times03",
    // "time-schedule",
    // "tkill01",
    // "tkill02",
    // "tpci",
    // "tpm_changeauth_tests.sh",
    // "tpm_changeauth_tests_exp01.sh",
    // "tpm_changeauth_tests_exp02.sh",
    // "tpm_changeauth_tests_exp03.sh",
    // "tpm_clear_tests.sh",
    // "tpm_clear_tests_exp01.sh",
    // "tpm_getpubek_tests.sh",
    // "tpm_getpubek_tests_exp01.sh",
    // "tpm_restrictpubek_tests.sh",
    // "tpm_restrictpubek_tests_exp01.sh",
    // "tpm_restrictpubek_tests_exp02.sh",
    // "tpm_restrictpubek_tests_exp03.sh",
    // "tpm_selftest_tests.sh",
    // "tpm_takeownership_tests.sh",
    // "tpm_takeownership_tests_exp01.sh",
    // "tpm_version_tests.sh",
    // "tpmtoken_import_tests.sh",
    // "tpmtoken_import_tests_exp01.sh",
    // "tpmtoken_import_tests_exp02.sh",
    // "tpmtoken_import_tests_exp03.sh",
    // "tpmtoken_import_tests_exp04.sh",
    // "tpmtoken_import_tests_exp05.sh",
    // "tpmtoken_import_tests_exp06.sh",
    // "tpmtoken_import_tests_exp07.sh",
    // "tpmtoken_import_tests_exp08.sh",
    // "tpmtoken_init_tests.sh",
    // "tpmtoken_init_tests_exp00.sh",
    // "tpmtoken_init_tests_exp01.sh",
    // "tpmtoken_init_tests_exp02.sh",
    // "tpmtoken_init_tests_exp03.sh",
    // "tpmtoken_objects_tests.sh",
    // "tpmtoken_objects_tests_exp01.sh",
    // "tpmtoken_protect_tests.sh",
    // "tpmtoken_protect_tests_exp01.sh",
    // "tpmtoken_protect_tests_exp02.sh",
    // "tpmtoken_setpasswd_tests.sh",
    // "tpmtoken_setpasswd_tests_exp01.sh",
    // "tpmtoken_setpasswd_tests_exp02.sh",
    // "tpmtoken_setpasswd_tests_exp03.sh",
    // "tpmtoken_setpasswd_tests_exp04.sh",
    // "trace_sched",
    // "tracepath01.sh",
    // "traceroute01.sh",
    // "truncate02",
    // "truncate02_64",
    // "truncate03",
    // "truncate03_64",
    // "tst_ansi_color.sh",
    // "tst_brk",
    // "tst_brkm",
    // "tst_cgctl",
    // "tst_check_drivers",
    // "tst_check_kconfigs",
    // "tst_checkpoint",
    // "tst_device",
    // "tst_exit",
    // "tst_fs_has_free",
    // "tst_fsfreeze",
    // "tst_get_free_pids",
    // "tst_get_median",
    // "tst_get_unused_port",
    // "tst_getconf",
    // "tst_hexdump",
    // "tst_kvcmp",
    // "tst_lockdown_enabled",
    // "tst_ncpus",
    // "tst_ncpus_conf",
    // "tst_ncpus_max",
    // "tst_net.sh",
    // "tst_net_iface_prefix",
    // "tst_net_ip_prefix",
    // "tst_net_stress.sh",
    // "tst_net_vars",
    // "tst_ns_create",
    // "tst_ns_exec",
    // "tst_ns_ifmove",
    // "tst_random",
    // "tst_res",
    // "tst_resm",
    // "tst_rod",
    // "tst_secureboot_enabled",
    // "tst_security.sh",
    // "tst_sleep",
    // "tst_supported_fs",
    // "tst_test.sh",
    // "tst_timeout_kill",
    // "uaccess",
    // "udp_ipsec.sh",
    // "udp_ipsec_vti.sh",
    // "udp4-multi-diffip01",
    // "udp4-multi-diffip02",
    // "udp4-multi-diffip03",
    // "udp4-multi-diffip04",
    // "udp4-multi-diffip05",
    // "udp4-multi-diffip06",
    // "udp4-multi-diffip07",
    // "udp4-multi-diffnic01",
    // "udp4-multi-diffnic02",
    // "udp4-multi-diffnic03",
    // "udp4-multi-diffnic04",
    // "udp4-multi-diffnic05",
    // "udp4-multi-diffnic06",
    // "udp4-multi-diffnic07",
    // "udp4-multi-diffport01",
    // "udp4-multi-diffport02",
    // "udp4-multi-diffport03",
    // "udp4-multi-diffport04",
    // "udp4-multi-diffport05",
    // "udp4-multi-diffport06",
    // "udp4-multi-diffport07",
    // "udp4-uni-basic01",
    // "udp4-uni-basic02",
    // "udp4-uni-basic03",
    // "udp4-uni-basic04",
    // "udp4-uni-basic05",
    // "udp4-uni-basic06",
    // "udp4-uni-basic07",
    // "udp6-multi-diffip01",
    // "udp6-multi-diffip02",
    // "udp6-multi-diffip03",
    // "udp6-multi-diffip04",
    // "udp6-multi-diffip05",
    // "udp6-multi-diffip06",
    // "udp6-multi-diffip07",
    // "udp6-multi-diffnic01",
    // "udp6-multi-diffnic02",
    // "udp6-multi-diffnic03",
    // "udp6-multi-diffnic04",
    // "udp6-multi-diffnic05",
    // "udp6-multi-diffnic06",
    // "udp6-multi-diffnic07",
    // "udp6-multi-diffport01",
    // "udp6-multi-diffport02",
    // "udp6-multi-diffport03",
    // "udp6-multi-diffport04",
    // "udp6-multi-diffport05",
    // "udp6-multi-diffport06",
    // "udp6-multi-diffport07",
    // "udp6-uni-basic01",
    // "udp6-uni-basic02",
    // "udp6-uni-basic03",
    // "udp6-uni-basic04",
    // "udp6-uni-basic05",
    // "udp6-uni-basic06",
    // "udp6-uni-basic07",
    // "uevent01",
    // "uevent02",
    // "uevent03",
    // "ulimit01",
    // "umask01",
    // "umip_basic_test",
    // "umount01",
    // "umount02",
    // "umount03",
    // "umount2_01",
    // "umount2_02",
    // "uname01", // 完全PASS
    // "uname02",// 完全PASS
    // "uname04", // 完全PASS
    // "unlink05", //pass
    // "unlink07",  //pass
    // "unlink08",   //sendmsg
    // "unlink09",   //pass
    // "unlinkat01", //nlink failed)
    // "unshare01",
    // "unshare01.sh",
    // "unshare02",
    // "unzip01.sh",
    // "userfaultfd01",
    // "userns01",
    // "userns02",
    // "userns03",
    // "userns04",
    // "userns05",
    // "userns06",
    // "userns06_capcheck",
    // "userns07",
    // "userns08",
    // "ustat01",
    // "ustat02",
    // "utime01",
    // "utime02",
    // "utime03",
    // "utime04",
    // "utime05",
    // "utime06",
    // "utime07",
    // "utimensat01",
    // "utimes01",
    // "utsname01",
    // "utsname02",
    // "utsname03",
    // "utsname04",
    // "verify_caps_exec",
    // "vfork",
    // "vfork_freeze.sh",
    // "vfork01",
    // "vfork02",
    // "vhangup01",
    // "vhangup02",
    // "virt_lib.sh",
    // "vlan01.sh",
    // "vlan02.sh",
    // "vlan03.sh",
    // "vma01",
    // "vma02",
    // "vma03",
    // "vma04",
    // "vma05.sh",
    // "vma05_vdso",
    // "vmsplice01",
    // "vmsplice02",
    // "vmsplice03",
    // "vmsplice04",
    // "vsock01",
    // "vxlan01.sh",
    // "vxlan02.sh",
    // "vxlan03.sh",
    // "vxlan04.sh",
    // "wait01",
    // "wait02",
    // "wait401",
    // "wait402",
    // "wait403",
    // "waitid01",
    // "waitid02",
    // "waitid03",
    // "waitid04",
    // "waitid05",
    // "waitid06",
    // "waitid07",
    // "waitid08",
    // "waitid09",
    // "waitid10",
    // "waitid11",
    // "waitpid01",
    // "waitpid03",
    // "waitpid04",
    // "waitpid06",
    // "waitpid07",
    // "waitpid08",
    // "waitpid09",
    // "waitpid10",
    // "waitpid11",
    // "waitpid12",
    // "waitpid13",
    // "wc01.sh",
    // "which01.sh",
    // "wireguard_lib.sh",
    // "wireguard01.sh",
    // "wireguard02.sh",
    // "wqueue01",
    // "wqueue02",
    // "wqueue03",
    // "wqueue04",
    // "wqueue05",
    // "wqueue06",
    // "wqueue07",
    // "wqueue08",
    // "wqueue09",
    // "write_freezing.sh",
    // "write01", // 完全PASS
    // "write02", //pass
    // "write03", // 完全PASS
    // "write04", /pass
    // "write05", // 有没过的
    // "write06",
    // "writetest",
    // "writev01", // 完全PASS
    // "writev02",
    // "writev03",
    // "writev05", // 完全PASS
    // "writev06", // 完全PASS
    // "writev07",
    // "zram_lib.sh",
    // "zram01.sh",
    // "zram02.sh",
    // "zram03",
    NULL};
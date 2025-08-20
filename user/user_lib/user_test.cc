
#include "user.hh"

extern char *libctest[][2];

const char musl_dir[] = "/musl/";
const char glibc_dir[] = "/glibc/";

// LTP测例结构体：{测例名字，riscv是否测试，龙芯是否测试}
struct ltp_testcase
{
    const char *name;
    bool test_riscv;
    bool test_loongarch;
};

extern struct ltp_testcase ltp_testcases[];

extern char *git_testcases[][8];

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
        if (waitpid(pid, &child_exit_state, 0) < 0)
            printf("wait fail\n");
    }
    return 0;
}

void init_env(const char *path = musl_dir)
{

    char *bb_sh[8] = {0};
    bb_sh[0] = "/bin/busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "-c";
    bb_sh[3] = "/bin/busybox --install /bin";
    run_test("busybox", bb_sh, 0);
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
    run_test("mount");  // todo
    run_test("umount"); // todo
    run_test("munmap");
    run_test("unlink");
    run_test("pipe");
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
    if (path == musl_dir)
        printf("#### OS COMP TEST GROUP START iozone-musl ####\n");
    else
        printf("#### OS COMP TEST GROUP START iozone-glibc ####\n");
    printf("iozone automatic measurements\n");
    run_test("iozone", bb_sh, 0);
    if (path == musl_dir)
        printf("#### OS COMP TEST GROUP end iozone-musl ####\n");
    else
        printf("#### OS COMP TEST GROUP end iozone-glibc ####\n");
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

int ltp_test(bool is_musl)
{
    chdir(is_musl ? "/musl/ltp/testcases/bin" : "/glibc/ltp/testcases/bin");
    printf("#### OS COMP TEST GROUP START ltp-%s ####\n", is_musl ? "musl" : "glibc");
    char *bb_sh[8] = {0};
    char *envp[] = {
        "PATH=/bin", // 设置 PATH
        "LD_LIBRARY_PATH=/glibc/lib",
        NULL // 必须以 NULL 结尾
    }; // 这个测loop的那些测例要用
    int result = 0;

    // 检测当前平台
#ifdef LOONGARCH
    bool is_loongarch = true;
#else
    bool is_loongarch = false;
#endif

    for (int i = 0; ltp_testcases[i].name != NULL; i++)
    {
        // 根据平台决定是否跳过测例
        if (is_loongarch && !ltp_testcases[i].test_loongarch)
        {
            printf("SKIP LTP CASE %s (disabled for LoongArch)\n", ltp_testcases[i].name);
            continue;
        }
        if (!is_loongarch && !ltp_testcases[i].test_riscv)
        {
            printf("SKIP LTP CASE %s (disabled for RISC-V)\n", ltp_testcases[i].name);
            continue;
        }

        printf("RUN LTP CASE %s\n", ltp_testcases[i].name);
        bb_sh[0] = (char *)ltp_testcases[i].name;
        result = run_test(ltp_testcases[i].name, bb_sh, envp);
        printf("FAIL LTP CASE %s: %d\n", ltp_testcases[i].name, result);
    }
    printf("#### OS COMP TEST GROUP END ltp-%s ####\n", is_musl ? "musl" : "glibc");
    return 0;
}

int git_test(const char *path)
{
    chdir(path);
    char *envp[] = {
        "HOME=/proj", // 设置 HOME
        NULL          // 必须以 NULL 结尾
    };
    for (int i = 0; git_testcases[i][0] != NULL; i++)
    {
        run_test(git_testcases[i][0], git_testcases[i], envp);
    }
    // char *bb_sh[8] = {0};
    // bb_sh[0] = "busybox";
    // bb_sh[1] = "sh";
    // bb_sh[2] = "git_testcode.sh";
    // run_test("busybox", bb_sh, envp);
    return 0;
}
int gcc_test()
{
    char *bb_sh[8] = {0};
    bb_sh[0] = "/usr/bin/gcc";
    bb_sh[1] = "--h";
    run_test("/usr/bin/gcc", bb_sh, 0);
    return 0;
}

int rustc_test()
{
    char *bb_sh[2] = {0};
    bb_sh[0] = "/usr/bin/rustc";
    bb_sh[1] = "-V";
    run_test("/usr/bin/rustc", bb_sh, 0);
}

int vim_h()
{
    char *bb_sh[2] = {0};
    bb_sh[0] = "usr/bin/vim";
    bb_sh[1] = "-h";
    run_test("usr/bin/vim", bb_sh, 0);
    return 0;
}

char *git_testcases[][8] = {
    {"/bin/busybox", "echo", "=============== Task0 BEGIN git -h ===============", NULL},
    {"/usr/bin/git", "help", NULL},
    {"/bin/busybox", "echo", "=============== Task0 END git -h ===============", NULL},
    {"/bin/busybox", "echo", "=============== Task1 BEGIN file ===============", NULL},
    {"/usr/bin/git", "config", "--global", "--add", "safe.directory", "$(pwd)", NULL},
    {"/usr/bin/git", "config", "--global", "user.email", "you@example.com", NULL},
    {"/usr/bin/git", "config", "--global", "user.name", "Your Name", NULL},
    {"/usr/bin/git", "init", NULL},
    {"/usr/bin/git", "add", ".", NULL},
    {"/usr/bin/git", "commit", "-m", "add README.md", NULL},
    {"/usr/bin/git", "log", NULL},
    {"/bin/busybox", "echo", "=============== Task1 END file ===============", NULL},
    {NULL}};

char *libctest[][2] = {
    {"argv", NULL},
    {"basename", NULL},
    {"clocale_mbfuncs", NULL},
    {"clock_gettime", NULL},
    {"dirname", NULL},
    {"env", NULL},
    {"fdopen", NULL}, // fdopen failed 问题在于写入后读不出来，怀疑根本没写入成功
    {"fnmatch", NULL},
    {"fscanf", NULL},  // ioctl 爆了
    {"fwscanf", NULL}, // 死了
    {"iconv_open", NULL},
    {"inet_pton", NULL},
    {"mbc", NULL},
    {"memstream", NULL},
    {"pthread_cancel_points", NULL}, // sig， fork高级用法
    {"pthread_cancel", NULL},        // sig， fork高级用法
    {"pthread_cond", NULL},          // sig， fork高级用法
    {"pthread_tsd", NULL},           // sig， fork高级用法
    {"qsort", NULL},
    {"random", NULL},
    {"search_hsearch", NULL},
    {"search_insque", NULL},
    {"search_lsearch", NULL},
    {"search_tsearch", NULL},
    {"setjmp", NULL}, // 信号相关，爆了
    {"snprintf", NULL},
    // // // {"socket", NULL}, // 网络相关，这个不测了
    {"sscanf", NULL},
    // {"sscanf_long", NULL}, // 龙芯会爆，riscv正常
    // {"stat", NULL},        // sys_fstatat我关掉了，原来就是关的，开了basictest爆炸，应该没实现对
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
    {"ungetc", NULL},
    // // // {"utime", NULL}, // sys_utimensat实现不正确
    {"wcsstr", NULL},
    {"wcstol", NULL},
    {"daemon_failure", NULL},
    {"dn_expand_empty", NULL},
    {"dn_expand_ptr_0", NULL},
    // // // {"fflush_exit", NULL},//fd爆了，标准输出不见了
    {"fgets_eof", NULL},
    {"fgetwc_buffering", NULL},
    {"fpclassify_invalid_ld80", NULL},
    {"ftello_unflushed_append", NULL},
    {"getpwnam_r_crash", NULL},
    {"getpwnam_r_errno", NULL},
    {"iconv_roundtrips", NULL},
    {"inet_ntop_v4mapped", NULL},
    {"inet_pton_empty_last_field", NULL},
    {"iswspace_null", NULL},
    {"lrand48_signextend", NULL},
    {"lseek_large", NULL},
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
    {"pthread_cancel_sem_wait", NULL}, // sig， fork高级用法
    {"pthread_cond_smasher", NULL},    // sig， fork高级用法
    // {"pthread_condattr_setclock", NULL}, // sig， fork高级用法
    {"pthread_exit_cancel", NULL},   // sig， fork高级用法
    {"pthread_once_deadlock", NULL}, // sig， fork高级用法
    {"pthread_rwlock_ebusy", NULL},  // sig， fork高级用法
    {"putenv_doublefree", NULL},
    {"regex_backref_0", NULL},
    {"regex_bracket_icase", NULL},
    {"regex_ere_backref", NULL},
    {"regex_escaped_high_byte", NULL},
    {"regex_negated_range", NULL},
    {"regexec_nosub", NULL},
    // // // {"rewind_clear_error", NULL}, // 爆了
    // // // {"rlimit_open_files", NULL}, // 爆了
    {"scanf_bytes_consumed", NULL},
    {"scanf_match_literal_eof", NULL},
    {"scanf_nullbyte_char", NULL},
    {"setvbuf_unget", NULL}, // streamdevice not support lseek currently!但是pass了
    {"sigprocmask_internal", NULL},
    {"sscanf_eof", NULL},
    {"statvfs", NULL},
    {"strverscmp", NULL},
    {"syscall_sign_extend", NULL},
    {"uselocale_0", NULL},
    {"wcsncpy_read_overflow", NULL},
    {"wcsstr_false_negative", NULL},
    {NULL}};

struct ltp_testcase ltp_testcases[] = {
    // 示例：{测例名字, riscv是否测试, 龙芯是否测试}
    // {"setresgid01", true, true}, // 先等等
    // {"setresgid02", true, true}, // 先等等
    // {"setresgid03", true, true}, // 先等等
    // {"mkdir02", true, true}, // 先等等
    // {"mkdir03", true, true}, // 先等等
    // {"mkdir04", true, true}, // 先等等
    // {"mkdir05", true, true}, // 先等等

    // {"getsid01", true, true}, // 先等等
    // {"getsid02", true, true}, // 先等等

    // {"getuid01", true, true},
    // {"getuid03", true, true},

    // {"setresuid01", true, true},
    // {"setresuid01_16", true, true},
    // {"setresuid02", true, true},
    // {"setresuid02_16", true, true},
    // {"setresuid03", true, true},
    // {"setresuid03_16", true, true},
    // {"setresuid04", true, true},
    // {"setresuid04_16", true, true},
    // {"setresuid05", true, true},
    // {"setresuid05_16", true, true},
    // {"setsid01", true, true},

    {NULL, true, true},
    {"memfd_create01", true, true},
    {"splice07", true, true},
    {"epoll_ctl03", true, true},
    {"access01", true, true},
    {"access02", true, true},
    {"access03", true, false},
    {"access04", true, true},
    {"getpid01", true, true},
    {"waitpid01", true, true}, // PASS
    {"timer_settime01", true, true},
    {"timer_settime02", true, true},
    {"clock_getres01", true, true},
    {"clock_gettime02", true, true}, // pass
    {"getitimer01", true, true},
    {"getitimer02", true, true},
    {"select01", true, true},
    {"select03", true, true},
    {"chmod01", true, true},
    {"chmod03", true, true}, // pass 4
    {"chmod06", true, true}, //   pass4 fail 5
    // "chmod07", true, true}, // pass4 fail 5,现在貌似fail了
    {"confstr01", true, true},
    {"creat01", true, true},         // passed   6
    {"creat06", true, true},         // pass
    {"posix_fadvise01", true, true}, // pass6
    {"posix_fadvise02", true, true}, // pass6
    {"posix_fadvise03", true, true},
    {"posix_fadvise01_64", true, true}, // pass6
    {"posix_fadvise02_64", true, true}, // pass6
    {"posix_fadvise03_64", true, true},
    {"signal03", true, true},
    {"signal04", true, true},
    {"signal05", true, true},
    {"add_key01", true, true},
    {"add_key02", true, true},
    {"add_key03", true, true},
    {"add_key04", true, true},
    {"accept01", true, true},
    {"accept03", true, true},
    {"dup01", true, true},            // 完全PASS
    {"dup02", true, true},            // 完全PASS
    {"dup03", true, true},            // 完全PASS
    {"dup04", true, true},            // 完全PASS
    {"dup05", true, true},            // pass
    {"dup06", true, true},            // 完全PASS
    {"dup07", true, true},            // 完全PASS
    {"dup201", true, true},           // 完全PASS
    {"dup202", true, true},           // 完全PASS
    {"dup203", true, true},           // pass
    {"dup204", true, true},           // 完全PASS
    {"dup205", true, true},           // 完全PASS
    {"dup206", true, true},           // 完全PASS
    {"epoll_create01", true, true},   // pass 2 skip 1
    {"epoll_create1_01", true, true}, // pass 1 skip 1
    {"execv01", true, true},          // 完全PASS
    {"execve01", true, true},         // 完全PASS
    {"fchdir01", true, true},         // 完全PASS
    {"fchdir02", true, true},         // 完全PASS
    {"fchmod01", true, true},         // pass
    {"fchmod03", true, true},         // pass
    {"fchmod04", true, true},         // pass
    {"fchmodat01", true, true},       // pass6
    {"fchmodat02", true, true},       // pass5 fail1
    {"fchown01", true, true},         // pass
    {"fchown02", true, true},         // pass 2 fail 1
    {"fchown03", true, true},         // pass
    {"fchown04", true, true},         // pass 2 fail 1
    {"fchown05", true, true},         // passed   6
    {"fcntl02", true, true},          // pass
    {"fcntl03", true, true},          // pass
    {"fcntl04", true, true},          // pass
    {"fcntl05", true, true},          // pass
    {"fcntl08", true, true},          // pass
    {"fcntl09", true, true},          // pass
    {"fcntl10", true, true},          // pass
    {"fcntl13", true, false},         // pass // la 会把用户态printf干爆
    {"fcntl15", true, true},          // passs5
    {"fcntl02_64", true, true},       // pass
    {"fcntl03_64", true, true},       // pass
    {"fcntl04_64", true, true},       // pass
    {"fcntl05_64", true, true},       // pass
    {"fcntl08_64", true, true},       // pass
    {"fcntl09_64", true, true},       // pass
    {"fcntl10_64", true, true},       // pass
    {"fcntl13_64", true, false},      // pass // la 会把用户态printf干爆
    {"fcntl15_64", true, true},       // passs5
    {"fstat02", true, true},          // pass 5 fail 1
    {"fstat03", true, false},         // pass2
    {"fstat02_64", true, true},       // pass 5 fail 1
    {"fstat03_64", true, false},      // pass2
    {"fstatfs02", true, false},       // pass 2
    {"fstatfs02_64", true, true},     // pass 2
    {"ftruncate01", true, true},      // pass 2
    {"ftruncate01_64", true, true},   // pass 2
    {"ftruncate03", true, true},      // pass 4
    {"faccessat01", true, true},      // 完全PASS
    {"faccessat02", true, true},      // 完全PASS
    {"faccessat201", true, true},     // pass
    {"setrlimit04", true, true},      // p1
    {"flock01", true, true},          // pass 3
    {"flock02", true, true},          // pass 3
    {"flock03", true, true},          // pass1 fail2 brok 1
    {"flock04", true, true},          // pass5 fail1
    {"flock06", true, true},          // pass2 fail 2
    {"flistxattr01", true, true},     // pass 1
    {"flistxattr02", true, true},     // pass 2
    {"flistxattr03", true, true},     // pass 2
    {"fpathconf01", true, true},      // pass
    {"fsync02", true, false},         // pass
    {"fsync03", true, true},          // pass
    {"kill03", true, true},           // pass
    {"kill11", true, true},           // pass
    {"waitpid03", true, true},        // PASS
    {"waitpid04", true, true},        // PASS
    {"waitpid06", true, true},        // PASS
    {"waitpid07", true, true},        // PASS
    {"waitpid09", true, true},        // 部分pass p3 f1
    {"getcwd01", true, true},         // pass
    {"getcwd02", true, true},         // 完全PASS
    {"getcwd03", true, false},        // pass
    {"getpgid01", true, true},        // PASS
    {"getpgid02", true, true},        // PASS
    {"getpid02", true, true},         // PASS
    {"getppid01", true, true},        // PASS
    {"getppid02", true, true},        // PASS
    {"getgid01", true, true},         // PASS
    {"getgid03", true, true},         // PASS
    {"getsid01", true, true},         // PASS
    {"getsid02", true, true},         // PASS
    {"getuid01", true, true},         // PASS
    {"getuid03", true, true},         // PASS
    {"setgid01", true, true},         // PASS
    {"setgid02", true, true},         // PASS
    {"setgid03", true, true},         // PASS
    {"setresgid01", true, true},      // 先等等
    {"setresgid02", true, true},      // 先等等
    {"setresgid03", true, true},      // 先等等
    {"setresgid04", true, true},      // 先等等
    {"setreuid01", true, true},       // PASS
    {"setreuid02", true, true},       // PASS
    {"setreuid03", true, true},       // PASS
    {"setreuid04", true, true},       // PASS
    {"setreuid05", true, true},       // PASS
    {"setreuid06", true, true},       // PASS
    {"setreuid07", true, true},       // p1 f2
    {"setregid01", true, true},       // PASS
    {"setregid02", true, true},       // PASS
    {"setregid03", true, true},       // PASS
    {"setregid04", true, true},       // PASS
    {"setegid01", true, true},        // PASS
    {"setegid02", true, true},        // PASS
    {"setfsgid01", true, true},       // p2 f1
    {"setfsgid02", true, true},       // PASS
    {"setfsuid01", true, true},       // PASS
    {"setfsuid03", true, true},       // PASS
    {"getpgrp01", true, true},        // PASS
    {"setpgrp01", true, true},        // PASS
    {"setpgrp02", true, true},        // PASS
    {"setuid01", true, true},         // PASS
    {"setuid03", true, true},         // PASS
    {"setresuid01", true, true},      // PASS
    {"setresuid02", true, true},      // PASS
    {"setresuid03", true, true},      // PASS
    {"setresuid04", true, true},      // p1 f2
    {"setresuid05", true, true},      // PASS
    {"getegid01", true, true},        // PASS
    {"getegid02", true, true},        // PASS
    {"geteuid01", true, true},        // PASS
    {"geteuid02", true, true},        // PASS
    {"clone01", true, true},          // pass
    {"clone03", true, true},          // pass
    {"clone06", true, true},          // pass
    {"clone302", true, true},         // p3 f5 s1
    {"getrandom01", true, true},      // pass
    {"getrandom02", true, true},      // 完全PASS
    {"getrandom03", true, true},      // 完全PASS
    {"getrandom04", true, true},      // 完全PASS
    {"getrandom05", true, true},      // pass
    {"getrlimit01", true, true},      // passed   16
    {"gettimeofday01", true, true},   // pass
    {"link02", true, true},           // pass
    {"link04", true, true},           // pass9 fail 5
    {"link08", true, true},           // pass3 fail1
    {"llseek01", true, true},         // pass
    {"llseek02", true, true},         // pass
    {"llseek03", true, true},         // pass
    {"lseek01", true, true},          // passed   4
    {"lseek02", true, true},          // passed   15
    {"lseek07", true, true},          // pass
    {"lstat01", true, true},
    {"lstat01_64", true, true},
    {"lstat02", true, true},
    {"lstat02_64", true, true},
    {"madvise01", true, true}, // pass
    {"madvise05", true, true},
    {"madvise10", true, true},
    {"mkdirat02", true, true}, // pass2fail2
    {"mkdir03", true, true},   // pass
    {"mknod02", true, true},
    {"mknod09", true, true},
    {"mmap02", true, true},
    {"mmap05", true, true},       // pass1 但是panic关了一个
    {"mmap06", true, true},       // pass6 fail 2
    {"mmap08", true, true},       // pass
    {"mmap09", true, true},       // pass
    {"mmap13", true, true},       // pass
    {"mmap15", true, true},       // pass
    {"mmap17", true, true},       // pass
    {"mmap19", true, true},       // pass
    {"mmap20", true, true},       // pass
    {"open01", true, true},       // pass
    {"open02", true, true},       // pass1 fail1
    {"open03", true, true},       // 完全PASS
    {"open04", true, true},       // 完全PASS
    {"open06", true, true},       // pass
    {"open07", true, true},       // pass
    {"open08", true, true},       // p4 f2
    {"open09", true, true},       // pass
    {"open10", true, true},       // p6 f3
    {"openat01", true, true},     // pass
    {"pathconf01", true, true},   // pass
    {"pathconf02", true, true},   // pass1 fail5
    {"pipe01", true, true},       // 完全PASS
    {"pipe03", true, true},       // 完全PASS
    {"pipe06", true, true},       // 完全PASS
    {"pipe10", true, true},       // 完全PASS
    {"pipe12", true, true},       // pass
    {"pipe14", true, true},       // 完全PASS
    {"exit02", true, true},       // pass
    {"poll01", true, true},       // pass
    {"pread01", true, true},      // pass
    {"pread01_64", true, true},   // pass
    {"pselect02", true, true},    // pass
    {"pselect02_64", true, true}, // pass
    {"pselect03", true, true},    // pass
    {"pselect03_64", true, true}, // pass
    {"pwrite01", true, true},     // pass
    {"pwrite01_64", true, true},  // pass
    {"read01", true, true},       // 貌似可以PASS
    {"read02", true, true},       // pass
    {"read03", true, true},
    {"read04", true, true},     // 完全PASS
    {"readlink01", true, true}, // pass 2
    {"readlink03", true, true}, // pass
    // "readlinkat01", true, true}, // pass 现在好像爆了
    {"readlinkat02", true, true}, // pass五个
    {"readv01", true, true},      // pass
    {"readv02", true, true},      // pass4 fail1
    {"rmdir01", true, true},      // pass
    {"rmdir02", true, true},      // pass
    {"rmdir03", true, true},      // fail2
    {"shmat01", true, true},      // pass4
    {"shmat03", true, true},      // pass?
    {"shmat04", true, true},      // pass
    {"shmctl02", true, true},     // passed   16 fail 4
    {"shmctl07", true, true},     // pass
    {"shmctl08", true, true},     // pass
    {"shmdt01", true, true},      // pass 2
    {"shmdt02", true, true},      // pass
    {"stat01", true, true},       // passed   12
    {"stat03", true, true},       // pass4 fail2
    {"stat01_64", true, true},    // passed   12
    {"stat03_64", true, false},   // pass4 fail2
    {"statfs02", true, false},    // pass3fail3
    {"statfs02_64", true, false}, // pass3fail3
    {"statx01", true, true},      // pass8 fail2
    {"statx02", true, true},      // pass4 fail1
    {"statx03", true, true},      // pass6 fail1
    {"symlink02", true, true},    // pass
    {"symlink03", true, true},    // sendmsg
    {"symlink04", true, true},    // pass
    {"syscall01", true, true},    // pass
    {"socket01", true, true},     // pass
    {"socket02", true, true},     // pass
    {"time01", true, true},       // pass
    {"truncate02", true, true},
    {"truncate02_64", true, true},
    {"truncate03", true, true},
    {"truncate03_64", true, true},
    {"uname01", true, true},     // 完全PASS
    {"uname02", true, true},     // 完全PASS
    {"unlink05", true, false},   // pass
    {"unlink07", true, false},   // pass
    {"unlink08", true, false},   // pass2fail2
    {"unlink09", true, false},   // pass
    {"unlinkat01", true, false}, // passed   7
    {"write01", true, false},    // 完全PASS
    {"write02", true, false},    // pass
    {"write03", true, false},    // 完全PASS
    {"write04", true, false},
    {"write05", true, false},  // passed   3
    {"writev05", true, false}, // 完全PASS
    {"writev06", true, false}, // 完全PASS
    {"execl01", true, true},   // PASS
    {"execle01", true, true},  // PASS
    {"execlp01", true, true},  // PASS
    {"execv01", true, true},   // PASS
    {"execve01", true, true},  // PASS
    {"execvp01", true, true},  // PASS
    {"gettid01", true, false}, // PASS
    {"set_tid_address01", true, false},
    {NULL, false, false}};

// 简单的交互式shell
int interactive_shell()
{
    printf("F7LY OS Interactive Shell\n");
    printf("Type 'help' for available commands, 'exit' to quit\n\n");
    
    char input_buffer[256];
    char *args[32];
    
    while (1) {
        printf("F7LY> ");
        
        // 读取用户输入
        if (read(0, input_buffer, sizeof(input_buffer)-1) <= 0) {
            continue;
        }
        
        // 去除换行符
        int len = 0;
        while (input_buffer[len] != '\0' && len < 255) len++;
        if (len > 0 && input_buffer[len-1] == '\n') {
            input_buffer[len-1] = '\0';
        }
        
        // 跳过空行
        if (input_buffer[0] == '\0') {
            continue;
        }
        
        // 简单的命令解析
        int argc = 0;
        char *current = input_buffer;
        while (*current && argc < 31) {
            // 跳过空格
            while (*current == ' ' || *current == '\t') current++;
            if (*current == '\0') break;
            
            args[argc++] = current;
            
            // 找到下一个空格或字符串结尾
            while (*current && *current != ' ' && *current != '\t') current++;
            if (*current) {
                *current = '\0';
                current++;
            }
        }
        args[argc] = 0;
        
        if (argc == 0) {
            continue;
        }
        
        // 处理内置命令
        if (strcmp(args[0], "exit") == 0) {
            printf("Goodbye!\n");
            break;
        } else if (strcmp(args[0], "help") == 0) {
            printf("Available commands:\n");
            printf("  help     - Show this help\n");
            printf("  exit     - Exit shell\n");
            printf("  cd <dir> - Change directory\n");
            printf("  ls       - List directory contents\n");
            printf("  cat <file> - Display file contents\n");
            printf("  echo <text> - Echo text\n");
            printf("  pwd      - Print working directory\n");
            printf("  Any other command will be executed if available\n");
        } else if (strcmp(args[0], "cd") == 0) {
            if (argc < 2) {
                printf("cd: missing argument\n");
            } else {
                if (chdir(args[1]) != 0) {
                    printf("cd: cannot change directory to '%s'\n", args[1]);
                }
            }
        } else if (strcmp(args[0], "pwd") == 0) {
            char cwd[256];
            if (getcwd(cwd, sizeof(cwd)) != 0) {
                printf("%s\n", cwd);
            } else {
                printf("pwd: error getting current directory\n");
            }
        } else if (strcmp(args[0], "echo") == 0) {
            for (int i = 1; i < argc; i++) {
                printf("%s", args[i]);
                if (i < argc - 1) printf(" ");
            }
            printf("\n");
        } else {
            // 尝试执行外部命令
            printf("Executing: %s", args[0]);
            for (int i = 1; i < argc; i++) {
                printf(" %s", args[i]);
            }
            printf("\n");
            
            int pid = fork();
            if (pid == 0) {
                // 子进程执行命令
                execve(args[0], args, 0);
                // 如果execve失败，尝试在busybox中执行
                char busybox_path[256];
                char *prefix = "/musl/usr/bin/";
                int i = 0;
                // 复制前缀
                while (prefix[i] != '\0' && i < 240) {
                    busybox_path[i] = prefix[i];
                    i++;
                }
                // 复制命令名
                int j = 0;
                while (args[0][j] != '\0' && i < 255) {
                    busybox_path[i++] = args[0][j++];
                }
                busybox_path[i] = '\0';
                execve(busybox_path, args, 0);
                
                // 如果都失败了
                printf("Error: command '%s' not found\n", args[0]);
                exit(1);
            } else if (pid > 0) {
                // 父进程等待子进程完成
                int status;
                wait(&status);
            } else {
                printf("Error: failed to fork\n");
            }
        }
    }
    
    return 0;
}

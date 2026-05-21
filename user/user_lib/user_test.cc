
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

static char **ltp_envp(bool is_musl)
{
    // musl/glibc 的测试程序都依赖同名的 libc.so。
    // 这里必须按运行时目录分别设置搜索路径，不能把 musl 测例错误地导向 glibc/libc.so。
    static char *musl_envp[] = {
        (char *)"PATH=/bin",
        (char *)"LD_LIBRARY_PATH=/musl/lib",
        NULL};
    static char *glibc_envp[] = {
        (char *)"PATH=/bin",
        (char *)"LD_LIBRARY_PATH=/glibc/lib",
        NULL};
    return is_musl ? musl_envp : glibc_envp;
}

size_t strlen(const char *s) noexcept(true)
{
    size_t len = 0;
    while (*s)
        s++, len++;
    return len;
}

static int decode_wait_status(int raw_status, bool &exited_normally)
{
    exited_normally = (raw_status & 0x7f) == 0;
    if (exited_normally)
    {
        return (raw_status >> 8) & 0xff;
    }
    return -(raw_status & 0x7f);
}

static int change_dir_checked(const char *path)
{
    int ret = chdir(path);
    if (ret != 0)
    {
        printf("[FAIL] chdir(%s) 失败: %d\n", path, ret);
    }
    return ret;
}

int run_test(const char *path, char *argv[], char *envp[])
{
    char *default_argv[2] = {0};
    if (argv == 0)
    {
        default_argv[0] = (char *)path;
        argv = default_argv;
    }

    printf("[RUN ] %s\n", path);
    int pid = fork();
    if (pid < 0)
    {
        printf("[FAIL] %s: fork 失败\n", path);
        return -1;
    }
    else if (pid == 0)
    {
        int exec_ret = execve(path, argv, envp);
        if (exec_ret < 0)
        {
            printf("[FAIL] %s: execve 失败 ret=%d\n", path, exec_ret);
            exit(127);
        }
        exit(127);
    }
    else
    {
        int child_exit_state = -1;
        if (waitpid(pid, &child_exit_state, 0) < 0)
        {
            // printf("[FAIL] %s: waitpid 失败\n", path);
            return -1;
        }

        bool exited_normally = false;
        int result = decode_wait_status(child_exit_state, exited_normally);
        if (result == 0)
        {
            printf("[PASS] %s (exit=0)\n", path);
            return 0;
        }

        if (exited_normally)
        {
            printf("[FAIL] %s (exit=%d, raw=0x%x)\n", path, result, child_exit_state);
        }
        else
        {
            printf("[FAIL] %s (signal=%d, raw=0x%x)\n", path, -result, child_exit_state);
        }
        return result;
    }
    return -1;
}

void init_env(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return;
    }

    // busybox 的 applet 依赖 /bin 作为安装目标。
    // 这里先兜底创建目录，避免 --install 时因为目标目录不存在而整批失败。
    int mkdir_ret = mkdir("/bin", 0777);
    if (mkdir_ret != 0 && mkdir_ret != -17)
    {
        printf("[FAIL] mkdir(/bin) 失败: %d\n", mkdir_ret);
        return;
    }

    char *bb_install[8] = {0};
    bb_install[0] = (char *)"busybox";
    bb_install[1] = (char *)"--install";
    bb_install[2] = (char *)"/bin";
    // 直接执行 busybox 自带的安装逻辑，避免再套一层 sh -c，
    // 这样 LoongArch 上即使 shell/管道/等待链路还有问题，也不会卡死在环境初始化阶段。
    run_test("busybox", bb_install, 0);
}

int basic_test(const char *path = musl_dir)
{
    [[maybe_unused]] int pid;
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    if (change_dir_checked("basic") != 0)
    {
        return -1;
    }
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

static int run_case_list_in_dir(const char *dir, const char *group_name, const char *const cases[], char *envp[])
{
    if (dir == 0 || cases == 0)
    {
        printf("[FAIL] %s: 参数为空\n", group_name ? group_name : "run_case_list_in_dir");
        return -1;
    }

    if (change_dir_checked(dir) != 0)
    {
        return -1;
    }

    int fail_count = 0;
    char *argv[2] = {0};
    if (group_name != 0)
    {
        printf("#### OS COMP TEST GROUP START %s ####\n", group_name);
    }
    for (int i = 0; cases[i] != 0; ++i)
    {
        argv[0] = (char *)cases[i];
        printf("RUN CASE %s\n", cases[i]);
        if (run_test(cases[i], argv, envp) != 0)
        {
            fail_count++;
        }
    }
    if (group_name != 0)
    {
        printf("#### OS COMP TEST GROUP END %s (fail=%d) ####\n", group_name, fail_count);
    }
    return fail_count;
}

int basic_subset_test(const char *path, const char *const cases[])
{
    if (path == 0 || cases == 0)
    {
        return -1;
    }

    const char *group_name = strcmp(path, musl_dir) == 0 ? "basic-subset-musl" : "basic-subset-glibc";

    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    if (change_dir_checked("basic") != 0)
    {
        return -1;
    }
    return run_case_list_in_dir(".", group_name, cases, 0);
}

int busybox_test(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "busybox_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int libcbench_test(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "libcbench_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int iozone_test(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
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
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
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
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
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
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "lmbench_testcode.sh";
    run_test("busybox", bb_sh, 0);
    return 0;
}

int ltp_test(bool is_musl)
{
    if (change_dir_checked(is_musl ? "/musl/ltp/testcases/bin" : "/glibc/ltp/testcases/bin") != 0)
    {
        return -1;
    }
    printf("#### OS COMP TEST GROUP START ltp-%s ####\n", is_musl ? "musl" : "glibc");
    char *bb_sh[8] = {0};
    char **envp = ltp_envp(is_musl);
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
        if (result != 0)
        {
            printf("FAIL LTP CASE %s: %d\n", ltp_testcases[i].name, result);
        }
    }
    printf("#### OS COMP TEST GROUP END ltp-%s ####\n", is_musl ? "musl" : "glibc");
    return 0;
}

int ltp_subset_test(bool is_musl, const char *const cases[])
{
    return run_case_list_in_dir(
        is_musl ? "/musl/ltp/testcases/bin" : "/glibc/ltp/testcases/bin",
        is_musl ? "ltp-subset-musl" : "ltp-subset-glibc",
        cases,
        ltp_envp(is_musl));
}

int regression_suite_4d1444_riscv(void)
{
    printf("#### REGRESSION START commit-4d1444b-riscv ####\n");
    init_env("/musl/");
    basic_test("/musl/");
    basic_test("/glibc/");
    ltp_test(true);
    ltp_test(false);
    busybox_test("/musl/");
    busybox_test("/glibc/");
    libc_test("/musl/");
    lua_test("/musl/");
    lua_test("/glibc/");
    libcbench_test("/musl");
    libcbench_test("/glibc");
    printf("#### REGRESSION END commit-4d1444b-riscv ####\n");
    return 0;
}

int regression_suite_4d1444_loongarch(void)
{
    printf("#### REGRESSION START commit-4d1444b-loongarch ####\n");
    init_env("/musl/");
    basic_test("/musl/");
    basic_test("/glibc/");
    ltp_test(true);
    ltp_test(false);
    busybox_test("/musl/");
    busybox_test("/glibc/");
    libc_test("/musl/");
    lua_test("/musl/");
    lua_test("/glibc/");
    libcbench_test("/musl");
    libcbench_test("/glibc");
    printf("#### REGRESSION END commit-4d1444b-loongarch ####\n");
    return 0;
}

int git_test(const char *path)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }
    char *envp[] = {
        "HOME=/musl", // 设置 HOME
        NULL          // 必须以 NULL 结尾
    };
    // for (int i = 0; git_testcases[i][0] != NULL; i++)
    // {
    //     run_test(git_testcases[i][0], git_testcases[i], envp);
    // }
    char *bb_sh[8] = {0};
    bb_sh[0] = "busybox";
    bb_sh[1] = "sh";
    bb_sh[2] = "git_testcode_old.sh";
    run_test("busybox", bb_sh, envp);
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
    // {"busybox", "echo", "#### OS COMP TEST GROUP START git-musl ####", NULL},
    // {"usr/bin/git", "config", "--global", "--add", "safe.directory", "$(pwd)", NULL},
    // {"usr/bin/git", "config", "--global", "user.email", "you@example.com", NULL},
    // {"usr/bin/git", "config", "--global", "user.name", "Your Name", NULL},
    // {"usr/bin/git", "help", NULL}, // ok
    // {"usr/bin/git", "init", NULL},
    // {"busybox", "echo", "hello world > README.md", NULL},
    // {"usr/bin/git", "add", "README.md", NULL},
    // {"usr/bin/git", "commit", "-m", "add README.md", NULL},
    // {"usr/bin/git", "log", NULL},
    // {"busybox", "echo", "#### OS COMP TEST GROUP END git-musl ####", NULL},
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
    // {"pthread_once_deadlock", NULL}, // sig， fork高级用法
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
    // 约定：第一个 {NULL, false, false} 就是当前默认跑测例的结束标记。
    // 下面继续保留的注释清单只作为候选记录，想打开哪个测例就把它挪到结束标记前面。
    // 新开以前完全没跑过的测例时，优先按 ltp_judge/ltp_rank.txt 的 total count 从高到低推进。
    {"personality02", true, true},
    {NULL, false, false},
    {"memfd_create01", true, true},
    {"splice07", true, true},
    {"epoll_ctl03", true, true},
    {"access01", true, true},
    {"access02", true, true},
    {"access03", true, false},
    {"access04", true, true},
    {"getpid01", true, true},
    {"pipe11", true, true}, // 2026-05-21: 四组合定向复测通过，total=70
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
    {"link04", true, true},           // 2026-05-21: link 语义修正后四组合定向复测通过，total=14
    {"link08", true, true},           // 2026-05-21: link 权限/前缀错误码修正后四组合定向复测通过，total=4
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
    {"open10", true, true},       // 2026-05-21: 目录 setgid 继承语义修正后四组合定向复测通过，total=9
    {"open11", true, true},       // 2026-05-21: 四组合定向复测通过，total=28
    {"openat01", true, true},     // pass
    {"pathconf01", true, true},   // pass
    {"pathconf02", true, true},   // pass1 fail5
    {"personality01", true, true}, // 2026-05-21: personality(2) 补齐后四组合定向复测通过，total=18
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
    {"readlink03", true, true}, // 2026-05-21: readlink 坏地址返回 EFAULT 后四组合定向复测通过，total=8
    {"readlinkat02", true, true}, // pass五个
    {"readv01", true, true},      // pass
    {"readv02", true, true},      // pass4 fail1
    {"rmdir01", true, true},      // pass
    {"rmdir02", true, true},      // pass
    {"rmdir03", true, true},      // 2026-05-21: sticky 目录删除权限语义修正后四组合定向复测通过，total=2
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
    {"symlink03", true, true},    // 2026-05-21: symlink 空路径/权限语义修正后四组合定向复测通过，total=0
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

    {NULL, false, false}, // 已验证并默认随回归运行的测例，到这里结束

    // 当前工作区里已经登记、但暂不默认开启的测例。
    // {"mkdir02", true, true}, // 先等等
    // {"mkdir04", true, true}, // 先等等
    // {"mkdir05", true, true}, // 先等等
    // {"setresuid01_16", true, true},
    // {"setresuid02_16", true, true},
    // {"setresuid03_16", true, true},
    // {"setresuid04_16", true, true},
    // {"setresuid05_16", true, true},
    // {"setsid01", true, true},
    // {"chmod07", true, true}, // pass4 fail 5,现在貌似fail了
    // {"readlinkat01", true, true}, // pass 现在好像爆了

    // 以下补齐历史完整 LTP 清单，默认全部保持注释状态。

    // {"stream01", true, true}, // pass
    // {"stream02", true, true}, // pass
    // {"stream03", true, true}, // pass
    // {"stream04", true, true}, // pass
    // {"stream05", true, true}, // pass
    // {"symlink01", true, true}, // pass
    // {"abort01", true, true},
    // {"abs01", true, true}, // 完全PASS,没summary
    // {"accept02", true, true},
    // {"accept4_01", true, true},
    // {"acct01", true, true},
    // {"acct02", true, true},
    // {"acct02_helper", true, true},
    // {"acl1", true, true},
    // {"add_ipv6addr", true, true},
    // {"add_key05", true, true},
    // {"adjtimex01", true, true},
    // {"adjtimex02", true, true},
    // {"adjtimex03", true, true},
    // {"af_alg01", true, true},
    // {"af_alg02", true, true},
    // {"af_alg03", true, true},
    // {"af_alg04", true, true},
    // {"af_alg05", true, true},
    // {"af_alg06", true, true},
    // {"af_alg07", true, true},
    // {"aio01", true, true},
    // {"aio02", true, true},
    // {"aiocp", true, true},
    // {"aiodio_append", true, true},
    // {"aiodio_sparse", true, true},
    // {"aio-stress", true, true},
    // {"alarm02", true, true},
    // {"alarm03", true, true},
    // {"alarm05", true, true},
    // {"alarm06", true, true},
    // {"alarm07", true, true},
    // {"ar01.sh", true, true},
    // {"arch_prctl01", true, true},
    // {"arping01.sh", true, true},
    // {"asapi_01", true, true}, // PASS一部分
    // {"asapi_02", true, true},
    // {"asapi_03", true, true},
    // {"ask_password.sh", true, true},
    // {"aslr01", true, true},
    // {"assign_password.sh", true, true},
    // {"atof01", true, true}, // PASS一部分
    // {"autogroup01", true, true},
    // {"bbr01.sh", true, true},
    // {"bbr02.sh", true, true},
    // {"bind_noport01.sh", true, true},
    // {"bind01", true, true},
    // {"bind02", true, true},
    // {"bind03", true, true},
    // {"bind04", true, true},
    // {"bind05", true, true},
    // {"bind06", true, true},
    // {"binfmt_misc_lib.sh", true, true},
    // {"binfmt_misc01.sh", true, true},
    // {"binfmt_misc02.sh", true, true},
    // {"block_dev", true, true},
    // {"bpf_map01", true, true},
    // {"bpf_prog01", true, true},
    // {"bpf_prog02", true, true},
    // {"bpf_prog03", true, true},
    // {"bpf_prog04", true, true},
    // {"bpf_prog05", true, true},
    // {"bpf_prog06", true, true},
    // {"bpf_prog07", true, true},
    // {"brk01", true, true},
    // {"brk02", true, true},
    // {"broken_ip-checksum.sh", true, true},
    // {"broken_ip-dstaddr.sh", true, true},
    // {"broken_ip-fragment.sh", true, true},
    // {"broken_ip-ihl.sh", true, true},
    // {"broken_ip-nexthdr.sh", true, true},
    // {"broken_ip-plen.sh", true, true},
    // {"broken_ip-protcol.sh", true, true},
    // {"broken_ip-version.sh", true, true},
    // {"busy_poll_lib.sh", true, true},
    // {"busy_poll01.sh", true, true},
    // {"busy_poll02.sh", true, true},
    // {"busy_poll03.sh", true, true},
    // {"cacheflush01", true, true},
    // {"can_bcm01", true, true},
    // {"can_filter", true, true},
    // {"can_rcv_own_msgs", true, true},
    // {"cap_bounds_r", true, true},
    // {"cap_bounds_rw", true, true},
    // {"cap_bset_inh_bounds", true, true},
    // {"capget01", true, true},
    // {"capget02", true, true},
    // {"capset01", true, true},
    // {"capset02", true, true},
    // {"capset03", true, true},
    // {"capset04", true, true},
    // {"cfs_bandwidth01", true, true},
    // {"cgroup_core01", true, true},
    // {"cgroup_core02", true, true},
    // {"cgroup_core03", true, true},
    // {"cgroup_fj_common.sh", true, true},
    // {"cgroup_fj_function.sh", true, true},
    // {"cgroup_fj_proc", true, true},
    // {"cgroup_fj_stress.sh", true, true},
    // {"cgroup_lib.sh", true, true},
    // {"cgroup_regression_3_1.sh", true, true},
    // {"cgroup_regression_3_2.sh", true, true},
    // {"cgroup_regression_5_1.sh", true, true},
    // {"cgroup_regression_5_2.sh", true, true},
    // {"cgroup_regression_6_1.sh", true, true},
    // {"cgroup_regression_6_2.sh", true, true},
    // {"cgroup_regression_fork_processes", true, true},
    // {"cgroup_regression_getdelays", true, true},
    // {"cgroup_regression_test.sh", true, true},
    // {"cgroup_xattr", true, true},
    // {"change_password.sh", true, true},
    // {"chdir01", true, true}, //  /dev/block/loop0
    // {"chdir04", true, true}, //pass 3
    // {"check_envval", true, true},
    // {"check_icmpv4_connectivity", true, true},
    // {"check_icmpv6_connectivity", true, true},
    // {"check_keepcaps", true, true},
    // {"check_netem", true, true},
    // {"check_pe", true, true},
    // {"check_setkey", true, true},
    // {"check_simple_capset", true, true},
    // {"chmod05", true, true}, //   setgroups未实现
    // {"chown01", true, true}, // pass
    // {"chown01_16", true, true},
    // {"chown02", true, true},
    // {"chown02_16", true, true},
    // {"chown03", true, true},
    // {"chown03_16", true, true},
    // {"chown04", true, true},
    // {"chown04_16", true, true},
    // {"chown05", true, true},
    // {"chown05_16", true, true},
    // {"chroot01", true, true},
    // {"chroot02", true, true},
    // {"chroot03", true, true},
    // {"chroot04", true, true},
    // {"cleanup_lvm.sh", true, true},
    // {"clock_adjtime01", true, true},
    // {"clock_adjtime02", true, true},
    // {"clock_gettime01", true, true},
    // {"clock_gettime03", true, true},
    // {"clock_gettime04", true, true},
    // {"clock_nanosleep01", true, true},
    // {"clock_nanosleep02", true, true},
    // {"clock_nanosleep03", true, true},
    // {"clock_nanosleep04", true, true},
    // {"clock_settime01", true, true},
    // {"clock_settime02", true, true},
    // {"clock_settime03", true, true},
    // {"clone02", true, true},
    // {"clone04", true, true},
    // {"clone05", true, true},
    // {"clone07", true, true},
    // {"clone08", true, true},
    // {"clone09", true, true},
    // {"clone301", true, true},
    // {"clone303", true, true},
    // {"close_range01", true, true},
    // {"close_range02", true, true},
    // {"close01", true, true},
    // {"close02", true, true},
    // {"cmdlib.sh", true, true},
    // {"cn_pec.sh", true, true},
    // {"connect01", true, true},
    // {"connect02", true, true},
    // {"copy_file_range01", true, true},
    // {"copy_file_range02", true, true},
    // {"copy_file_range03", true, true},
    // {"cp_tests.sh", true, true},
    // {"cpio_tests.sh", true, true},
    // {"cpuacct.sh", true, true},
    // {"cpuacct_task", true, true},
    // {"cpuctl_def_task01", true, true},
    // {"cpuctl_def_task02", true, true},
    // {"cpuctl_def_task03", true, true},
    // {"cpuctl_def_task04", true, true},
    // {"cpuctl_fj_cpu-hog", true, true},
    // {"cpuctl_fj_simple_echo", true, true},
    // {"cpuctl_latency_check_task", true, true},
    // {"cpuctl_latency_test", true, true},
    // {"cpuctl_test01", true, true},
    // {"cpuctl_test02", true, true},
    // {"cpuctl_test03", true, true},
    // {"cpuctl_test04", true, true},
    // {"cpufreq_boost", true, true},
    // {"cpuhotplug_do_disk_write_loop", true, true},
    // {"cpuhotplug_do_kcompile_loop", true, true},
    // {"cpuhotplug_do_spin_loop", true, true},
    // {"cpuhotplug_hotplug.sh", true, true},
    // {"cpuhotplug_report_proc_interrupts", true, true},
    // {"cpuhotplug_testsuite.sh", true, true},
    // {"cpuhotplug01.sh", true, true},
    // {"cpuhotplug02.sh", true, true},
    // {"cpuhotplug03.sh", true, true},
    // {"cpuhotplug04.sh", true, true},
    // {"cpuhotplug05.sh", true, true},
    // {"cpuhotplug06.sh", true, true},
    // {"cpuhotplug07.sh", true, true},
    // {"cpuset01", true, true},
    // {"crash01", true, true},
    // {"crash02", true, true}, //  acct未实现
    // {"creat03", true, true}, // pass
    // {"creat04", true, true}, // pass
    // {"creat05", true, true}, // pass
    // {"creat07", true, true}, //pass4 fail4 这个好像会trap
    // {"creat07_child", true, true},
    // {"creat08", true, true}, //group
    // {"creat09", true, true}, // /dev/block/loop0
    // {"create_datafile", true, true},
    // {"create_file", true, true},
    // {"crypto_user01", true, true}, //socket(16, 524290, 21) failed: EAFNOSUPPORT (97)
    // {"crypto_user02", true, true},
    // {"cve-2014-0196", true, true},
    // {"cve-2015-3290", true, true},
    // {"cve-2016-10044", true, true},
    // {"cve-2016-7042", true, true},
    // {"cve-2016-7117", true, true},
    // {"cve-2017-16939", true, true},
    // {"cve-2017-17052", true, true},
    // {"cve-2017-17053", true, true},
    // {"cve-2017-2618", true, true},
    // {"cve-2017-2671", true, true},
    // {"cve-2022-4378", true, true},
    // {"daemonlib.sh", true, true},
    // {"data", true, true},
    // {"data_space", true, true},
    // {"dccp_ipsec.sh", true, true},
    // {"dccp_ipsec_vti.sh", true, true},
    // {"dccp01.sh", true, true},
    // {"dctcp01.sh", true, true},
    // {"delete_module01", true, true},
    // {"delete_module02", true, true},
    // {"delete_module03", true, true},
    // {"df01.sh", true, true},
    // {"dhcp_lib.sh", true, true},
    // {"dhcpd_tests.sh", true, true},
    // {"dio_append", true, true},
    // {"dio_read", true, true},
    // {"dio_sparse", true, true},
    // {"dio_truncate", true, true},
    // {"diotest1", true, true},
    // {"diotest2", true, true},
    // {"diotest3", true, true},
    // {"diotest4", true, true},
    // {"diotest5", true, true},
    // {"diotest6", true, true},
    // {"dirty", true, true},
    // {"dirtyc0w", true, true},
    // {"dirtyc0w_child", true, true},
    // {"dirtyc0w_shmem", true, true},
    // {"dirtyc0w_shmem_child", true, true},
    // {"dirtypipe", true, true},
    // {"dma_thread_diotest", true, true},
    // {"dnsmasq_tests.sh", true, true},
    // {"dns-stress.sh", true, true},
    // {"dns-stress01-rmt.sh", true, true},
    // {"dns-stress02-rmt.sh", true, true},
    // {"dns-stress-lib.sh", true, true},
    // {"doio", true, true},
    // {"du01.sh", true, true},
    // {"dup207", true, true}, //
    // {"dup3_01", true, true}, //
    // {"dup3_02", true, true}, // 完全PASS
    // {"dynamic_debug01.sh", true, true},
    // {"ebizzy", true, true},
    // {"eject_check_tray", true, true},
    // {"eject-tests.sh", true, true},
    // {"endian_switch01", true, true},
    // {"epoll_create02", true, true},
    // {"epoll_create1_02", true, true},
    // {"epoll_ctl01", true, true},
    // {"epoll_ctl02", true, true},
    // {"epoll_ctl04", true, true},
    // {"epoll_ctl05", true, true},
    // {"epoll_pwait01", true, true},
    // {"epoll_pwait02", true, true},
    // {"epoll_pwait03", true, true},
    // {"epoll_pwait04", true, true},
    // {"epoll_pwait05", true, true},
    // {"epoll_wait01", true, true},
    // {"epoll_wait02", true, true},
    // {"epoll_wait03", true, true},
    // {"epoll_wait04", true, true},
    // {"epoll_wait05", true, true},
    // {"epoll_wait06", true, true},
    // {"epoll_wait07", true, true},
    // {"epoll-ltp", true, true},
    // {"event_generator", true, true},
    // {"eventfd01", true, true},
    // {"eventfd02", true, true},
    // {"eventfd03", true, true},
    // {"eventfd04", true, true},
    // {"eventfd05", true, true},
    // {"eventfd06", true, true},
    // {"eventfd2_01", true, true},
    // {"eventfd2_02", true, true},
    // {"eventfd2_03", true, true},
    // {"evm_overlay.sh", true, true},
    // {"exec_with_inh", true, true},
    // {"exec_without_inh", true, true},
    // {"execl01_child", true, true},
    // {"execle01_child", true, true},
    // {"execlp01_child", true, true},
    // {"execv01_child", true, true},
    // {"execve_child", true, true},
    // {"execve01_child", true, true},
    // {"execve02", true, true},
    // {"execve03", true, true},
    // {"execve04", true, true},
    // {"execve05", true, true},
    // {"execve06", true, true},
    // {"execve06_child", true, true},
    // {"execveat_child", true, true},
    // {"execveat_errno", true, true},
    // {"execveat01", true, true},
    // {"execveat02", true, true},
    // {"execveat03", true, true},
    // {"execvp01_child", true, true},
    // {"exit_group01", true, true},
    // {"exit01", true, true},
    // {"f00f", true, true},
    // {"faccessat202", true, true}, //涉及网络😭😭😭
    // {"fallocate01", true, true}, //过了一半
    // {"fallocate02", true, true}, // 完全通过
    // {"fallocate03", true, true}, //卡死了
    // {"fallocate04", true, true},
    // {"fallocate05", true, true},
    // {"fallocate06", true, true},
    // {"fanotify_child", true, true},
    // {"fanotify01", true, true},
    // {"fanotify02", true, true},
    // {"fanotify03", true, true},
    // {"fanotify04", true, true},
    // {"fanotify05", true, true},
    // {"fanotify06", true, true},
    // {"fanotify07", true, true},
    // {"fanotify08", true, true},
    // {"fanotify09", true, true},
    // {"fanotify10", true, true},
    // {"fanotify11", true, true},
    // {"fanotify12", true, true},
    // {"fanotify13", true, true},
    // {"fanotify14", true, true},
    // {"fanotify15", true, true},
    // {"fanotify16", true, true},
    // {"fanotify17", true, true},
    // {"fanotify18", true, true},
    // {"fanotify19", true, true},
    // {"fanotify20", true, true},
    // {"fanotify21", true, true},
    // {"fanotify22", true, true},
    // {"fanotify23", true, true},
    // {"fanout01", true, true},
    // {"fchdir03", true, true}, // fail
    // {"fchmod02", true, true}, //  /etc/group
    // {"fchmod05", true, true}, //爆了
    // {"fchmod06", true, true}, //pass1 fail2
    // {"fchown01_16", true, true},
    // {"fchown02_16", true, true},
    // {"fchown03_16", true, true},
    // {"fchown04_16", true, true},
    // {"fchown05_16", true, true},
    // {"fchownat01", true, true}, //pass但是没summary
    // {"fchownat02", true, true}, ////pass但是没summary
    // {"fcntl01", true, true},
    // {"fcntl01_64", true, true},
    // {"fcntl07", true, true},
    // {"fcntl07_64", true, true},
    // {"fcntl11", true, true},
    // {"fcntl11_64", true, true},
    // {"fcntl12", true, true}, //fail
    // {"fcntl12_64", true, true}, //fail
    // {"fcntl14", true, true}, //rt_sigsuspend
    // {"fcntl14_64", true, true}, //rt_sigsuspend
    // {"fcntl16", true, true},
    // {"fcntl16_64", true, true},
    // {"fcntl17", true, true},
    // {"fcntl17_64", true, true},
    // {"fcntl18", true, true},
    // {"fcntl18_64", true, true},
    // {"fcntl19", true, true},
    // {"fcntl19_64", true, true},
    // {"fcntl20", true, true},
    // {"fcntl20_64", true, true},
    // {"fcntl21", true, true},
    // {"fcntl21_64", true, true},
    // {"fcntl22", true, true},
    // {"fcntl22_64", true, true},
    // {"fcntl23", true, true},
    // {"fcntl23_64", true, true},
    // {"fcntl24", true, true},
    // {"fcntl24_64", true, true},
    // {"fcntl25", true, true},
    // {"fcntl25_64", true, true},
    // {"fcntl26", true, true},
    // {"fcntl26_64", true, true},
    // {"fcntl27", true, true},
    // {"fcntl27_64", true, true},
    // {"fcntl29", true, true},
    // {"fcntl29_64", true, true},
    // {"fcntl30", true, true},
    // {"fcntl30_64", true, true},
    // {"fcntl31", true, true},
    // {"fcntl31_64", true, true},
    // {"fcntl32", true, true},
    // {"fcntl32_64", true, true},
    // {"fcntl33", true, true},
    // {"fcntl33_64", true, true},
    // {"fcntl34", true, true},
    // {"fcntl34_64", true, true},
    // {"fcntl35", true, true},
    // {"fcntl35_64", true, true},
    // {"fcntl36", true, true},
    // {"fcntl36_64", true, true},
    // {"fcntl37", true, true},
    // {"fcntl37_64", true, true},
    // {"fcntl38", true, true},
    // {"fcntl38_64", true, true},
    // {"fcntl39", true, true},
    // {"fcntl39_64", true, true},
    // {"fdatasync01", true, true}, // pass
    // {"fdatasync02", true, true}, // pass
    // {"fdatasync03", true, true}, //loop0
    // {"fgetxattr01", true, true}, //bin/sh
    // {"fgetxattr02", true, true},
    // {"fgetxattr03", true, true},
    // {"file01.sh", true, true},
    // {"filecapstest.sh", true, true},
    // {"find_portbundle", true, true},
    // {"finit_module01", true, true},
    // {"finit_module02", true, true},
    // {"float_bessel", true, true},
    // {"float_exp_log", true, true},
    // {"float_iperb", true, true},
    // {"float_power", true, true},
    // {"float_trigo", true, true},
    // {"force_erase.sh", true, true},
    // {"fork_exec_loop", true, true},
    // {"fork_freeze.sh", true, true},
    // {"fork_procs", true, true}, // pass1 跑挺久
    // {"fork01", true, true}, //pass 2
    // {"fork03", true, true}, //pass 1
    // {"fork04", true, true}, //pass 3
    // {"fork05", true, true},
    // {"fork07", true, true},
    // {"fork08", true, true},
    // {"fork09", true, true},
    // {"fork10", true, true},
    // {"fork13", true, true},
    // {"fork14", true, true},
    // {"fou01.sh", true, true},
    // {"fptest01", true, true},
    // {"fptest02", true, true},
    // {"frag", true, true},
    // {"freeze_cancel.sh", true, true},
    // {"freeze_kill_thaw.sh", true, true},
    // {"freeze_move_thaw.sh", true, true},
    // {"freeze_self_thaw.sh", true, true},
    // {"freeze_sleep_thaw.sh", true, true},
    // {"freeze_thaw.sh", true, true},
    // {"freeze_write_freezing.sh", true, true},
    // {"fremovexattr01", true, true},
    // {"fremovexattr02", true, true},
    // {"fs_bind_cloneNS01.sh", true, true},
    // {"fs_bind_cloneNS02.sh", true, true},
    // {"fs_bind_cloneNS03.sh", true, true},
    // {"fs_bind_cloneNS04.sh", true, true},
    // {"fs_bind_cloneNS05.sh", true, true},
    // {"fs_bind_cloneNS06.sh", true, true},
    // {"fs_bind_cloneNS07.sh", true, true},
    // {"fs_bind_lib.sh", true, true},
    // {"fs_bind_move01.sh", true, true},
    // {"fs_bind_move02.sh", true, true},
    // {"fs_bind_move03.sh", true, true},
    // {"fs_bind_move04.sh", true, true},
    // {"fs_bind_move05.sh", true, true},
    // {"fs_bind_move06.sh", true, true},
    // {"fs_bind_move07.sh", true, true},
    // {"fs_bind_move08.sh", true, true},
    // {"fs_bind_move09.sh", true, true},
    // {"fs_bind_move10.sh", true, true},
    // {"fs_bind_move11.sh", true, true},
    // {"fs_bind_move12.sh", true, true},
    // {"fs_bind_move13.sh", true, true},
    // {"fs_bind_move14.sh", true, true},
    // {"fs_bind_move15.sh", true, true},
    // {"fs_bind_move16.sh", true, true},
    // {"fs_bind_move17.sh", true, true},
    // {"fs_bind_move18.sh", true, true},
    // {"fs_bind_move19.sh", true, true},
    // {"fs_bind_move20.sh", true, true},
    // {"fs_bind_move21.sh", true, true},
    // {"fs_bind_move22.sh", true, true},
    // {"fs_bind_rbind01.sh", true, true},
    // {"fs_bind_rbind02.sh", true, true},
    // {"fs_bind_rbind03.sh", true, true},
    // {"fs_bind_rbind04.sh", true, true},
    // {"fs_bind_rbind05.sh", true, true},
    // {"fs_bind_rbind06.sh", true, true},
    // {"fs_bind_rbind07.sh", true, true},
    // {"fs_bind_rbind07-2.sh", true, true},
    // {"fs_bind_rbind08.sh", true, true},
    // {"fs_bind_rbind09.sh", true, true},
    // {"fs_bind_rbind10.sh", true, true},
    // {"fs_bind_rbind11.sh", true, true},
    // {"fs_bind_rbind12.sh", true, true},
    // {"fs_bind_rbind13.sh", true, true},
    // {"fs_bind_rbind14.sh", true, true},
    // {"fs_bind_rbind15.sh", true, true},
    // {"fs_bind_rbind16.sh", true, true},
    // {"fs_bind_rbind17.sh", true, true},
    // {"fs_bind_rbind18.sh", true, true},
    // {"fs_bind_rbind19.sh", true, true},
    // {"fs_bind_rbind20.sh", true, true},
    // {"fs_bind_rbind21.sh", true, true},
    // {"fs_bind_rbind22.sh", true, true},
    // {"fs_bind_rbind23.sh", true, true},
    // {"fs_bind_rbind24.sh", true, true},
    // {"fs_bind_rbind25.sh", true, true},
    // {"fs_bind_rbind26.sh", true, true},
    // {"fs_bind_rbind27.sh", true, true},
    // {"fs_bind_rbind28.sh", true, true},
    // {"fs_bind_rbind29.sh", true, true},
    // {"fs_bind_rbind30.sh", true, true},
    // {"fs_bind_rbind31.sh", true, true},
    // {"fs_bind_rbind32.sh", true, true},
    // {"fs_bind_rbind33.sh", true, true},
    // {"fs_bind_rbind34.sh", true, true},
    // {"fs_bind_rbind35.sh", true, true},
    // {"fs_bind_rbind36.sh", true, true},
    // {"fs_bind_rbind37.sh", true, true},
    // {"fs_bind_rbind38.sh", true, true},
    // {"fs_bind_rbind39.sh", true, true},
    // {"fs_bind_regression.sh", true, true},
    // {"fs_bind01.sh", true, true},
    // {"fs_bind02.sh", true, true},
    // {"fs_bind03.sh", true, true},
    // {"fs_bind04.sh", true, true},
    // {"fs_bind05.sh", true, true},
    // {"fs_bind06.sh", true, true},
    // {"fs_bind07.sh", true, true},
    // {"fs_bind07-2.sh", true, true},
    // {"fs_bind08.sh", true, true},
    // {"fs_bind09.sh", true, true},
    // {"fs_bind10.sh", true, true},
    // {"fs_bind11.sh", true, true},
    // {"fs_bind12.sh", true, true},
    // {"fs_bind13.sh", true, true},
    // {"fs_bind14.sh", true, true},
    // {"fs_bind15.sh", true, true},
    // {"fs_bind16.sh", true, true},
    // {"fs_bind17.sh", true, true},
    // {"fs_bind18.sh", true, true},
    // {"fs_bind19.sh", true, true},
    // {"fs_bind20.sh", true, true},
    // {"fs_bind21.sh", true, true},
    // {"fs_bind22.sh", true, true},
    // {"fs_bind23.sh", true, true},
    // {"fs_bind24.sh", true, true},
    // {"fs_di", true, true},
    // {"fs_fill", true, true},
    // {"fs_inod", true, true},
    // {"fs_perms", true, true},
    // {"fs_racer.sh", true, true},
    // {"fs_racer_dir_create.sh", true, true},
    // {"fs_racer_dir_test.sh", true, true},
    // {"fs_racer_file_concat.sh", true, true},
    // {"fs_racer_file_create.sh", true, true},
    // {"fs_racer_file_link.sh", true, true},
    // {"fs_racer_file_list.sh", true, true},
    // {"fs_racer_file_rename.sh", true, true},
    // {"fs_racer_file_rm.sh", true, true},
    // {"fs_racer_file_symlink.sh", true, true},
    // {"fsconfig01", true, true}, ///dev/block/loop0
    // {"fsconfig02", true, true},
    // {"fsconfig03", true, true},
    // {"fsetxattr01", true, true},
    // {"fsetxattr02", true, true},
    // {"fsmount01", true, true},
    // {"fsmount02", true, true},
    // {"fsopen01", true, true}, ///dev/block/loop0
    // {"fsopen02", true, true},
    // {"fspick01", true, true}, ///dev/block/loop0
    // {"fspick02", true, true},
    // {"fsstress", true, true},
    // {"fstatat01", true, true}, //无summary
    // {"fstatfs01", true, true}, ///dev/loop0
    // {"fstatfs01_64", true, true},
    // {"fsx.sh", true, true},
    // {"fsx-linux", true, true},
    // {"fsync01", true, true}, ///dev/block/loop0
    // {"fsync04", true, true}, ///dev/block/loop0
    // {"ftest01", true, true},
    // {"ftest02", true, true},
    // {"ftest03", true, true},
    // {"ftest04", true, true},
    // {"ftest05", true, true},
    // {"ftest06", true, true},
    // {"ftest07", true, true},
    // {"ftest08", true, true},
    // {"ftp01.sh", true, true},
    // {"ftp-download-stress.sh", true, true},
    // {"ftp-download-stress01-rmt.sh", true, true},
    // {"ftp-download-stress02-rmt.sh", true, true},
    // {"ftp-upload-stress.sh", true, true},
    // {"ftp-upload-stress01-rmt.sh", true, true},
    // {"ftp-upload-stress02-rmt.sh", true, true},
    // {"ftrace_lib.sh", true, true},
    // {"ftrace_regression01.sh", true, true},
    // {"ftrace_regression02.sh", true, true},
    // {"ftrace_stress_test.sh", true, true},
    // {"ftruncate03_64", true, true},
    // {"ftruncate04", true, true},
    // {"ftruncate04_64", true, true},
    // {"futex_cmp_requeue01", true, true},
    // {"futex_cmp_requeue02", true, true},
    // {"futex_wait_bitset01", true, true},
    // {"futex_wait01", true, true},
    // {"futex_wait02", true, true},
    // {"futex_wait03", true, true},
    // {"futex_wait04", true, true},
    // {"futex_wait05", true, true},
    // {"futex_waitv01", true, true},
    // {"futex_waitv02", true, true},
    // {"futex_waitv03", true, true},
    // {"futex_wake01", true, true},
    // {"futex_wake02", true, true},
    // {"futex_wake03", true, true},
    // {"futex_wake04", true, true},
    // {"futimesat01", true, true},
    // {"fw_load", true, true},
    // {"gdb01.sh", true, true},
    // {"genacos", true, true},
    // {"genasin", true, true},
    // {"genatan", true, true},
    // {"genatan2", true, true},
    // {"genbessel", true, true},
    // {"genceil", true, true},
    // {"gencos", true, true},
    // {"gencosh", true, true},
    // {"generate_lvm_runfile.sh", true, true},
    // {"geneve01.sh", true, true},
    // {"geneve02.sh", true, true},
    // {"genexp", true, true},
    // {"genexp_log", true, true},
    // {"genfabs", true, true},
    // {"genfloor", true, true},
    // {"genfmod", true, true},
    // {"genfrexp", true, true},
    // {"genhypot", true, true},
    // {"geniperb", true, true},
    // {"genj0", true, true},
    // {"genj1", true, true},
    // {"genldexp", true, true},
    // {"genlgamma", true, true},
    // {"genload", true, true},
    // {"genlog", true, true},
    // {"genlog10", true, true},
    // {"genmodf", true, true},
    // {"genpow", true, true},
    // {"genpower", true, true},
    // {"gensin", true, true},
    // {"gensinh", true, true},
    // {"gensqrt", true, true},
    // {"gentan", true, true},
    // {"gentanh", true, true},
    // {"gentrigo", true, true},
    // {"geny0", true, true},
    // {"geny1", true, true},
    // {"get_ifname", true, true},
    // {"get_mempolicy01", true, true},
    // {"get_mempolicy02", true, true},
    // {"get_robust_list01", true, true},
    // {"getaddrinfo_01", true, true}, // 2026-05-21: 四组合定向复测为 TCONF，/etc/hosts 缺失
    // {"getcontext01", true, true},
    // {"getcpu01", true, true}, //sched_setaffinity
    // {"getcwd04", true, true}, // Test needs at least 2 CPUs online 这个是因为 sched_getaffinity返回0，说不定它不用两个CPU
    // {"getdents01", true, true},
    // {"getdents02", true, true},
    // {"getdomainname01", true, true}, // pass 1
    // {"getegid01_16", true, true},
    // {"getegid02_16", true, true},
    // {"geteuid01_16", true, true},
    // {"geteuid02_16", true, true},
    // {"getgid01_16", true, true},
    // {"getgid03_16", true, true},
    // {"getgroups01", true, true},
    // {"getgroups01_16", true, true},
    // {"getgroups03", true, true},
    // {"getgroups03_16", true, true},
    // {"gethostbyname_r01", true, true},
    // {"gethostid01", true, true},
    // {"gethostname01", true, true},
    // {"gethostname02", true, true},
    // {"getpagesize01", true, true},
    // {"getpeername01", true, true},
    // {"getpriority01", true, true},
    // {"getpriority02", true, true},
    // {"getresgid01", true, true},
    // {"getresgid01_16", true, true},
    // {"getresgid02", true, true},
    // {"getresgid02_16", true, true},
    // {"getresgid03", true, true},
    // {"getresgid03_16", true, true},
    // {"getresuid01", true, true},
    // {"getresuid01_16", true, true},
    // {"getresuid02", true, true},
    // {"getresuid02_16", true, true},
    // {"getresuid03", true, true},
    // {"getresuid03_16", true, true},
    // {"getrlimit02", true, true}, //爆了
    // {"getrlimit03", true, true}, // 2026-05-21: 四组合定向复测失败，__NR_getrlimit 仍返回 ENOSYS
    // {"getrusage01", true, true},
    // {"getrusage02", true, true},
    // {"getrusage03", true, true},
    // {"getrusage03_child", true, true},
    // {"getrusage04", true, true},
    // {"getsockname01", true, true},
    // {"getsockopt01", true, true},
    // {"getsockopt02", true, true},
    // {"gettid02", true, true}, // PASS
    // {"gettimeofday02", true, true},
    // {"getuid01_16", true, true},
    // {"getuid03_16", true, true},
    // {"getxattr01", true, true},
    // {"getxattr02", true, true},
    // {"getxattr03", true, true},
    // {"getxattr04", true, true},
    // {"getxattr05", true, true},
    // {"gre01.sh", true, true},
    // {"gre02.sh", true, true},
    // {"growfiles", true, true},
    // {"gzip_tests.sh", true, true},
    // {"hackbench", true, true},
    // {"hangup01", true, true},
    // {"ht_affinity", true, true},
    // {"ht_enabled", true, true},
    // {"http-stress.sh", true, true},
    // {"http-stress01-rmt.sh", true, true},
    // {"http-stress02-rmt.sh", true, true},
    // {"hugefallocate01", true, true},
    // {"hugefallocate02", true, true},
    // {"hugefork01", true, true},
    // {"hugefork02", true, true},
    // {"hugemmap01", true, true},
    // {"hugemmap02", true, true},
    // {"hugemmap04", true, true},
    // {"hugemmap05", true, true},
    // {"hugemmap06", true, true},
    // {"hugemmap07", true, true},
    // {"hugemmap08", true, true},
    // {"hugemmap09", true, true},
    // {"hugemmap10", true, true},
    // {"hugemmap11", true, true},
    // {"hugemmap12", true, true},
    // {"hugemmap13", true, true},
    // {"hugemmap14", true, true},
    // {"hugemmap15", true, true},
    // {"hugemmap16", true, true},
    // {"hugemmap17", true, true},
    // {"hugemmap18", true, true},
    // {"hugemmap19", true, true},
    // {"hugemmap20", true, true},
    // {"hugemmap21", true, true},
    // {"hugemmap22", true, true},
    // {"hugemmap23", true, true},
    // {"hugemmap24", true, true},
    // {"hugemmap25", true, true},
    // {"hugemmap26", true, true},
    // {"hugemmap27", true, true},
    // {"hugemmap28", true, true},
    // {"hugemmap29", true, true},
    // {"hugemmap30", true, true},
    // {"hugemmap31", true, true},
    // {"hugemmap32", true, true},
    // {"hugeshmat01", true, true},
    // {"hugeshmat02", true, true},
    // {"hugeshmat03", true, true},
    // {"hugeshmat04", true, true},
    // {"hugeshmat05", true, true},
    // {"hugeshmctl01", true, true},
    // {"hugeshmctl02", true, true},
    // {"hugeshmctl03", true, true},
    // {"hugeshmdt01", true, true},
    // {"hugeshmget01", true, true},
    // {"hugeshmget02", true, true},
    // {"hugeshmget03", true, true},
    // {"hugeshmget05", true, true},
    // {"icmp_rate_limit01", true, true},
    // {"icmp4-multi-diffip01", true, true},
    // {"icmp4-multi-diffip02", true, true},
    // {"icmp4-multi-diffip03", true, true},
    // {"icmp4-multi-diffip04", true, true},
    // {"icmp4-multi-diffip05", true, true},
    // {"icmp4-multi-diffip06", true, true},
    // {"icmp4-multi-diffip07", true, true},
    // {"icmp4-multi-diffnic01", true, true},
    // {"icmp4-multi-diffnic02", true, true},
    // {"icmp4-multi-diffnic03", true, true},
    // {"icmp4-multi-diffnic04", true, true},
    // {"icmp4-multi-diffnic05", true, true},
    // {"icmp4-multi-diffnic06", true, true},
    // {"icmp4-multi-diffnic07", true, true},
    // {"icmp6-multi-diffip01", true, true},
    // {"icmp6-multi-diffip02", true, true},
    // {"icmp6-multi-diffip03", true, true},
    // {"icmp6-multi-diffip04", true, true},
    // {"icmp6-multi-diffip05", true, true},
    // {"icmp6-multi-diffip06", true, true},
    // {"icmp6-multi-diffip07", true, true},
    // {"icmp6-multi-diffnic01", true, true},
    // {"icmp6-multi-diffnic02", true, true},
    // {"icmp6-multi-diffnic03", true, true},
    // {"icmp6-multi-diffnic04", true, true},
    // {"icmp6-multi-diffnic05", true, true},
    // {"icmp6-multi-diffnic06", true, true},
    // {"icmp6-multi-diffnic07", true, true},
    // {"icmp-uni-basic.sh", true, true},
    // {"icmp-uni-vti.sh", true, true},
    // {"if4-addr-change.sh", true, true},
    // {"if-addr-adddel.sh", true, true},
    // {"if-addr-addlarge.sh", true, true},
    // {"if-lib.sh", true, true},
    // {"if-mtu-change.sh", true, true},
    // {"if-route-adddel.sh", true, true},
    // {"if-route-addlarge.sh", true, true},
    // {"if-updown.sh", true, true},
    // {"ima_boot_aggregate", true, true},
    // {"ima_conditionals.sh", true, true},
    // {"ima_kexec.sh", true, true},
    // {"ima_keys.sh", true, true},
    // {"ima_measurements.sh", true, true},
    // {"ima_mmap", true, true},
    // {"ima_policy.sh", true, true},
    // {"ima_selinux.sh", true, true},
    // {"ima_setup.sh", true, true},
    // {"ima_tpm.sh", true, true},
    // {"ima_violations.sh", true, true},
    // {"in6_01", true, true},
    // {"in6_02", true, true},
    // {"inh_capped", true, true},
    // {"init_module01", true, true},
    // {"init_module02", true, true},
    // {"initialize_if", true, true},
    // {"inode01", true, true},
    // {"inode02", true, true},
    // {"inotify_init1_01", true, true},
    // {"inotify_init1_02", true, true},
    // {"inotify01", true, true},
    // {"inotify02", true, true},
    // {"inotify03", true, true},
    // {"inotify04", true, true},
    // {"inotify05", true, true},
    // {"inotify06", true, true},
    // {"inotify07", true, true},
    // {"inotify08", true, true},
    // {"inotify09", true, true},
    // {"inotify10", true, true},
    // {"inotify11", true, true},
    // {"inotify12", true, true},
    // {"input01", true, true},
    // {"input02", true, true},
    // {"input03", true, true},
    // {"input04", true, true},
    // {"input05", true, true},
    // {"input06", true, true},
    // {"insmod01.sh", true, true},
    // {"io_cancel01", true, true},
    // {"io_cancel02", true, true},
    // {"io_control01", true, true},
    // {"io_destroy01", true, true},
    // {"io_destroy02", true, true},
    // {"io_getevents01", true, true},
    // {"io_getevents02", true, true},
    // {"io_pgetevents01", true, true},
    // {"io_pgetevents02", true, true},
    // {"io_setup01", true, true},
    // {"io_setup02", true, true},
    // {"io_submit01", true, true},
    // {"io_submit02", true, true},
    // {"io_submit03", true, true},
    // {"io_uring01", true, true},
    // {"io_uring02", true, true},
    // {"ioctl_loop01", true, true},
    // {"ioctl_loop02", true, true},
    // {"ioctl_loop03", true, true},
    // {"ioctl_loop04", true, true},
    // {"ioctl_loop05", true, true},
    // {"ioctl_loop06", true, true},
    // {"ioctl_loop07", true, true},
    // {"ioctl_ns01", true, true},
    // {"ioctl_ns02", true, true},
    // {"ioctl_ns03", true, true},
    // {"ioctl_ns04", true, true},
    // {"ioctl_ns05", true, true},
    // {"ioctl_ns06", true, true},
    // {"ioctl_ns07", true, true},
    // {"ioctl_sg01", true, true},
    // {"ioctl01", true, true},
    // {"ioctl02", true, true},
    // {"ioctl03", true, true},
    // {"ioctl04", true, true},
    // {"ioctl05", true, true},
    // {"ioctl06", true, true},
    // {"ioctl07", true, true},
    // {"ioctl08", true, true},
    // {"ioctl09", true, true},
    // {"iogen", true, true},
    // {"ioperm01", true, true},
    // {"ioperm02", true, true},
    // {"iopl01", true, true},
    // {"iopl02", true, true},
    // {"ioprio_get01", true, true},
    // {"ioprio_set01", true, true},
    // {"ioprio_set02", true, true},
    // {"ioprio_set03", true, true},
    // {"ip_tests.sh", true, true},
    // {"ipneigh01.sh", true, true},
    // {"ipsec_lib.sh", true, true},
    // {"iptables_lib.sh", true, true},
    // {"iptables01.sh", true, true},
    // {"ipvlan01.sh", true, true},
    // {"irqbalance01", true, true},
    // {"isofs.sh", true, true},
    // {"kallsyms", true, true},
    // {"kcmp01", true, true},
    // {"kcmp02", true, true},
    // {"kcmp03", true, true},
    // {"kernbench", true, true},
    // {"keyctl01", true, true},
    // {"keyctl01.sh", true, true},
    // {"keyctl02", true, true},
    // {"keyctl03", true, true},
    // {"keyctl04", true, true},
    // {"keyctl05", true, true},
    // {"keyctl06", true, true},
    // {"keyctl07", true, true},
    // {"keyctl08", true, true},
    // {"keyctl09", true, true},
    // {"kill02", true, true},
    // {"kill05", true, true},
    // {"kill06", true, true},
    // {"kill07", true, true},
    // {"kill08", true, true},
    // {"kill09", true, true},
    // {"kill10", true, true},
    // {"kill12", true, true},
    // {"kill13", true, true},
    // {"killall_icmp_traffic", true, true},
    // {"killall_tcp_traffic", true, true},
    // {"killall_udp_traffic", true, true},
    // {"kmsg01", true, true},
    // {"ksm01", true, true},
    // {"ksm02", true, true},
    // {"ksm03", true, true},
    // {"ksm04", true, true},
    // {"ksm05", true, true},
    // {"ksm06", true, true},
    // {"ksm07", true, true},
    // {"lchown01", true, true},
    // {"lchown01_16", true, true},
    // {"lchown02", true, true},
    // {"lchown02_16", true, true},
    // {"lchown03", true, true},
    // {"lchown03_16", true, true},
    // {"ld01.sh", true, true},
    // {"ldd01.sh", true, true},
    // {"leapsec01", true, true},
    // {"lftest", true, true},
    // {"lgetxattr01", true, true},
    // {"lgetxattr02", true, true},
    // {"libcgroup_freezer", true, true},
    // {"link05", true, true}, //pass,这个也是逆天数量
    // {"linkat01", true, true}, //没summary
    // {"linkat02", true, true}, ///dev/block/loop0
    // {"linktest.sh", true, true},
    // {"listen01", true, true},
    // {"listxattr01", true, true},
    // {"listxattr02", true, true},
    // {"listxattr03", true, true},
    // {"llistxattr01", true, true},
    // {"llistxattr02", true, true},
    // {"llistxattr03", true, true},
    // {"ln_tests.sh", true, true},
    // {"lock_torture.sh", true, true},
    // {"locktests", true, true},
    // {"logrotate_tests.sh", true, true},
    // {"lremovexattr01", true, true},
    // {"lseek11", true, true}, //SEEK_DATA and SEEK_HOLE not implemented
    // {"lsmod01.sh", true, true},
    // {"ltp_acpi", true, true},
    // {"ltpClient", true, true},
    // {"ltpServer", true, true},
    // {"ltpSockets.sh", true, true},
    // {"macsec_lib.sh", true, true},
    // {"macsec01.sh", true, true},
    // {"macsec02.sh", true, true},
    // {"macsec03.sh", true, true},
    // {"macvlan01.sh", true, true},
    // {"macvtap01.sh", true, true},
    // {"madvise02", true, true},
    // {"madvise03", true, true},
    // {"madvise06", true, true},
    // {"madvise07", true, true},
    // {"madvise08", true, true},
    // {"madvise09", true, true},
    // {"madvise11", true, true},
    // {"mallinfo01", true, true},
    // {"mallinfo02", true, true},
    // {"mallinfo2_01", true, true},
    // {"mallocstress", true, true},
    // {"mallopt01", true, true},
    // {"max_map_count", true, true}, ///proc/sys/vm/overcommit_memory
    // {"mbind01", true, true},
    // {"mbind02", true, true},
    // {"mbind03", true, true},
    // {"mbind04", true, true},
    // {"mc_cmds.sh", true, true},
    // {"mc_commo.sh", true, true},
    // {"mc_member.sh", true, true},
    // {"mc_member_test", true, true},
    // {"mc_opts.sh", true, true},
    // {"mc_recv", true, true},
    // {"mc_send", true, true},
    // {"mc_verify_opts", true, true},
    // {"mc_verify_opts_error", true, true},
    // {"mcast-group-multiple-socket.sh", true, true},
    // {"mcast-group-same-group.sh", true, true},
    // {"mcast-group-single-socket.sh", true, true},
    // {"mcast-group-source-filter.sh", true, true},
    // {"mcast-lib.sh", true, true},
    // {"mcast-pktfld01.sh", true, true},
    // {"mcast-pktfld02.sh", true, true},
    // {"mcast-queryfld01.sh", true, true},
    // {"mcast-queryfld02.sh", true, true},
    // {"mcast-queryfld03.sh", true, true},
    // {"mcast-queryfld04.sh", true, true},
    // {"mcast-queryfld05.sh", true, true},
    // {"mcast-queryfld06.sh", true, true},
    // {"meltdown", true, true},
    // {"mem_process", true, true},
    // {"mem02", true, true}, //过了但是没有summary
    // {"membarrier01", true, true},
    // {"memcg_control_test.sh", true, true},
    // {"memcg_failcnt.sh", true, true},
    // {"memcg_force_empty.sh", true, true},
    // {"memcg_lib.sh", true, true},
    // {"memcg_limit_in_bytes.sh", true, true},
    // {"memcg_max_usage_in_bytes_test.sh", true, true},
    // {"memcg_memsw_limit_in_bytes_test.sh", true, true},
    // {"memcg_move_charge_at_immigrate_test.sh", true, true},
    // {"memcg_process", true, true},
    // {"memcg_process_stress", true, true},
    // {"memcg_regression_test.sh", true, true},
    // {"memcg_stat_rss.sh", true, true},
    // {"memcg_stat_test.sh", true, true},
    // {"memcg_stress_test.sh", true, true},
    // {"memcg_subgroup_charge.sh", true, true},
    // {"memcg_test_1", true, true},
    // {"memcg_test_2", true, true},
    // {"memcg_test_3", true, true},
    // {"memcg_test_4", true, true},
    // {"memcg_test_4.sh", true, true},
    // {"memcg_usage_in_bytes_test.sh", true, true},
    // {"memcg_use_hierarchy_test.sh", true, true},
    // {"memcmp01", true, true}, // pass 2
    // {"memcontrol01", true, true}, ///proc/self/mounts
    // {"memcontrol02", true, true}, ///dev/block/loop0
    // {"memcontrol03", true, true}, ///dev/block/loop0
    // {"memcontrol04", true, true}, ///dev/block/loop0连着跑似乎就变成loop1和loop2了
    // {"memcpy01", true, true}, // passed   2
    // {"memctl_test01", true, true},
    // {"memfd_create02", true, true},
    // {"memfd_create03", true, true},
    // {"memfd_create04", true, true},
    // {"memset01", true, true}, // passed   1
    // {"memtoy", true, true},
    // {"mesgq_nstest", true, true},
    // {"migrate_pages01", true, true},
    // {"migrate_pages02", true, true},
    // {"migrate_pages03", true, true},
    // {"min_free_kbytes", true, true}, ///proc/sys/vm/overcommit_memory
    // {"mincore01", true, true},
    // {"mincore02", true, true},
    // {"mincore03", true, true},
    // {"mincore04", true, true},
    // {"mkdir_tests.sh", true, true},
    // {"mkdir09", true, true}, //bin/sh
    // {"mkdirat01", true, true}, // pass
    // {"mkfs01.sh", true, true},
    // {"mknod01", true, true},
    // {"mknod03", true, true},
    // {"mknod04", true, true},
    // {"mknod05", true, true},
    // {"mknod06", true, true},
    // {"mknod07", true, true},
    // {"mknod08", true, true},
    // {"mknodat01", true, true},
    // {"mknodat02", true, true},
    // {"mkswap01.sh", true, true},
    // {"mlock01", true, true},
    // {"mlock02", true, true},
    // {"mlock03", true, true},
    // {"mlock04", true, true},
    // {"mlock05", true, true},
    // {"mlock201", true, true},
    // {"mlock202", true, true},
    // {"mlock203", true, true},
    // {"mlockall01", true, true},
    // {"mlockall02", true, true},
    // {"mlockall03", true, true},
    // {"mmap001", true, true}, // pass.
    // {"mmap01", true, true}, //bin/sh
    // {"mmap03", true, true}, //无所谓，没summary
    // {"mmap04", true, true},
    // {"mmap1", true, true},
    // {"mmap10", true, true}, //无所谓，没summary
    // {"mmap11", true, true}, //pass不能和别的一起跑
    // {"mmap12", true, true},
    // {"mmap14", true, true},
    // {"mmap16", true, true},
    // {"mmap18", true, true},
    // {"mmap2", true, true},
    // {"mmap3", true, true},
    // {"mmap-corruption01", true, true},
    // {"mmapstress01", true, true},
    // {"mmapstress02", true, true},
    // {"mmapstress03", true, true},
    // {"mmapstress04", true, true},
    // {"mmapstress05", true, true},
    // {"mmapstress06", true, true},
    // {"mmapstress07", true, true},
    // {"mmapstress08", true, true},
    // {"mmapstress09", true, true},
    // {"mmapstress10", true, true},
    // {"mmstress", true, true},
    // {"mmstress_dummy", true, true},
    // {"modify_ldt01", true, true},
    // {"modify_ldt02", true, true},
    // {"modify_ldt03", true, true},
    // {"mount_setattr01", true, true},
    // {"mount01", true, true}, ///dev/loop0
    // {"mount02", true, true},
    // {"mount03", true, true},
    // {"mount03_suid_child", true, true},
    // {"mount04", true, true},
    // {"mount05", true, true},
    // {"mount06", true, true},
    // {"mount07", true, true},
    // {"mountns01", true, true},
    // {"mountns02", true, true},
    // {"mountns03", true, true},
    // {"mountns04", true, true},
    // {"move_mount01", true, true},
    // {"move_mount02", true, true},
    // {"move_pages01", true, true},
    // {"move_pages02", true, true},
    // {"move_pages03", true, true},
    // {"move_pages04", true, true},
    // {"move_pages05", true, true},
    // {"move_pages06", true, true},
    // {"move_pages07", true, true},
    // {"move_pages09", true, true},
    // {"move_pages10", true, true},
    // {"move_pages11", true, true},
    // {"move_pages12", true, true},
    // {"mpls_lib.sh", true, true},
    // {"mpls01.sh", true, true},
    // {"mpls02.sh", true, true},
    // {"mpls03.sh", true, true},
    // {"mpls04.sh", true, true},
    // {"mprotect01", true, true}, //无所谓，没summary
    // {"mprotect02", true, true}, //无所谓，没summary
    // {"mprotect03", true, true}, //无所谓，没summary
    // {"mprotect04", true, true}, //无所谓，没summary
    // {"mprotect05", true, true}, //pass 1 fail1
    // {"mq_notify01", true, true},
    // {"mq_notify02", true, true},
    // {"mq_notify03", true, true},
    // {"mq_open01", true, true},
    // {"mq_timedreceive01", true, true}, // 2026-05-21: 四组合定向复测失败，mq_open 仍返回 ENOSYS
    // {"mq_timedsend01", true, true}, // 2026-05-21: 四组合定向复测失败，mq_open 仍返回 ENOSYS
    // {"mq_unlink01", true, true},
    // {"mqns_01", true, true},
    // {"mqns_02", true, true},
    // {"mqns_03", true, true},
    // {"mqns_04", true, true},
    // {"mremap01", true, true}, // pass 没summary
    // {"mremap02", true, true}, // pass 没summary
    // {"mremap03", true, true}, // pass 没summary
    // {"mremap04", true, true}, // pass 没summary
    // {"mremap05", true, true}, // pass 没summary
    // {"mremap06", true, true},
    // {"msg_comm", true, true},
    // {"msgctl01", true, true},
    // {"msgctl02", true, true},
    // {"msgctl03", true, true},
    // {"msgctl04", true, true},
    // {"msgctl05", true, true},
    // {"msgctl06", true, true},
    // {"msgctl12", true, true},
    // {"msgget01", true, true},
    // {"msgget02", true, true},
    // {"msgget03", true, true},
    // {"msgget04", true, true},
    // {"msgget05", true, true},
    // {"msgrcv01", true, true},
    // {"msgrcv02", true, true},
    // {"msgrcv03", true, true},
    // {"msgrcv05", true, true},
    // {"msgrcv06", true, true},
    // {"msgrcv07", true, true},
    // {"msgrcv08", true, true},
    // {"msgsnd01", true, true},
    // {"msgsnd02", true, true},
    // {"msgsnd05", true, true},
    // {"msgsnd06", true, true},
    // {"msgstress01", true, true},
    // {"msync01", true, true}, // pass
    // {"msync02", true, true}, // pass两个
    // {"msync03", true, true}, //pass
    // {"msync04", true, true}, ///dev/loop0
    // {"mtest01", true, true},
    // {"munlock01", true, true},
    // {"munlock02", true, true},
    // {"munlockall01", true, true},
    // {"munmap01", true, true}, // pass 没summary
    // {"munmap02", true, true}, // pass 没summary
    // {"munmap03", true, true}, // pass 没summary
    // {"mv_tests.sh", true, true},
    // {"myfunctions.sh", true, true},
    // {"name_to_handle_at01", true, true},
    // {"name_to_handle_at02", true, true},
    // {"nanosleep01", true, true},
    // {"nanosleep02", true, true},
    // {"nanosleep04", true, true},
    // {"net_cmdlib.sh", true, true},
    // {"netns_breakns.sh", true, true},
    // {"netns_comm.sh", true, true},
    // {"netns_lib.sh", true, true},
    // {"netns_netlink", true, true},
    // {"netns_sysfs.sh", true, true},
    // {"netstat01.sh", true, true},
    // {"netstress", true, true},
    // {"newuname01", true, true}, // pass 没summary
    // {"nextafter01", true, true},
    // {"nfs_flock", true, true},
    // {"nfs_flock_dgen", true, true},
    // {"nfs_lib.sh", true, true},
    // {"nfs01.sh", true, true},
    // {"nfs01_open_files", true, true},
    // {"nfs02.sh", true, true},
    // {"nfs03.sh", true, true},
    // {"nfs04.sh", true, true},
    // {"nfs04_create_file", true, true},
    // {"nfs05.sh", true, true},
    // {"nfs05_make_tree", true, true},
    // {"nfs06.sh", true, true},
    // {"nfs07.sh", true, true},
    // {"nfs08.sh", true, true},
    // {"nfs09.sh", true, true},
    // {"nfslock01.sh", true, true},
    // {"nfsstat01.sh", true, true},
    // {"nft01.sh", true, true},
    // {"nft02", true, true},
    // {"nftw01", true, true},
    // {"nftw6401", true, true},
    // {"nice01", true, true},
    // {"nice02", true, true},
    // {"nice03", true, true},
    // {"nice04", true, true},
    // {"nice05", true, true},
    // {"nm01.sh", true, true},
    // {"nptl01", true, true},
    // {"ns-echoclient", true, true},
    // {"ns-icmp_redirector", true, true},
    // {"ns-icmpv4_sender", true, true},
    // {"ns-icmpv6_sender", true, true},
    // {"ns-igmp_querier", true, true},
    // {"ns-mcast_join", true, true},
    // {"ns-mcast_receiver", true, true},
    // {"ns-tcpclient", true, true},
    // {"ns-tcpserver", true, true},
    // {"ns-udpclient", true, true},
    // {"ns-udpsender", true, true},
    // {"ns-udpserver", true, true},
    // {"numa01.sh", true, true},
    // {"oom01", true, true},
    // {"oom02", true, true},
    // {"oom03", true, true},
    // {"oom04", true, true},
    // {"oom05", true, true},
    // {"open_by_handle_at01", true, true},
    // {"open_by_handle_at02", true, true},
    // {"open_tree01", true, true},
    // {"open_tree02", true, true},
    // {"open12", true, true}, //过三个
    // {"open12_child", true, true}, //这个不是测例
    // {"open13", true, true}, // pass
    // {"open14", true, true}, //pass这个测例要跑一年，别急着掐死，多等会
    // {"openat02", true, true}, //爆了
    // {"openat02_child", true, true},
    // {"openat03", true, true}, //pass这个和那个一年是同一个
    // {"openat04", true, true}, ///dev/block/loop0
    // {"openat201", true, true},
    // {"openat202", true, true},
    // {"openat203", true, true},
    // {"openfile", true, true},
    // {"output_ipsec_conf", true, true},
    // {"overcommit_memory", true, true},
    // {"page01", true, true},
    // {"page02", true, true},
    // {"parameters.sh", true, true},
    // {"pause01", true, true},
    // {"pause02", true, true},
    // {"pause03", true, true},
    // {"pcrypt_aead01", true, true},
    // {"pec_listener", true, true},
    // {"perf_event_open01", true, true},
    // {"perf_event_open02", true, true},
    // {"perf_event_open03", true, true},
    // {"personality02", true, true},
    // {"pidfd_getfd01", true, true},
    // {"pidfd_getfd02", true, true},
    // {"pidfd_open01", true, true},
    // {"pidfd_open02", true, true},
    // {"pidfd_open03", true, true},
    // {"pidfd_open04", true, true},
    // {"pidfd_send_signal01", true, true},
    // {"pidfd_send_signal02", true, true},
    // {"pidfd_send_signal03", true, true},
    // {"pidns01", true, true},
    // {"pidns02", true, true},
    // {"pidns03", true, true},
    // {"pidns04", true, true},
    // {"pidns05", true, true},
    // {"pidns06", true, true},
    // {"pidns10", true, true},
    // {"pidns12", true, true},
    // {"pidns13", true, true},
    // {"pidns16", true, true},
    // {"pidns17", true, true},
    // {"pidns20", true, true},
    // {"pidns30", true, true},
    // {"pidns31", true, true},
    // {"pidns32", true, true},
    // {"pids.sh", true, true},
    // {"pids_task1", true, true},
    // {"pids_task2", true, true},
    // {"ping01.sh", true, true},
    // {"ping02.sh", true, true},
    // {"pipe02", true, true},
    // {"pipe04", true, true}, //管道给写爆了，感觉是时间片太长了
    // {"pipe05", true, true}, // 完全PASS
    // {"pipe07", true, true}, //proc/self/fd没写
    // {"pipe08", true, true},
    // {"pipe09", true, true}, // 完全PASS
    // {"pipe13", true, true}, // proc/4/stat没写
    // {"pipe15", true, true}, //NOFILE limit max too low: 128 < 65536
    // {"pipe2_01", true, true}, // pass
    // {"pipe2_02", true, true},
    // {"pipe2_02_child", true, true},
    // {"pipe2_04", true, true},
    // {"pipeio", true, true},
    // {"pivot_root01", true, true},
    // {"pkey01", true, true},
    // {"pm_cpu_consolidation.py", true, true},
    // {"pm_get_sched_values", true, true},
    // {"pm_ilb_test.py", true, true},
    // {"pm_include.sh", true, true},
    // {"pm_sched_domain.py", true, true},
    // {"pm_sched_mc.py", true, true},
    // {"poll02", true, true},
    // {"posix_fadvise04", true, true},
    // {"posix_fadvise04_64", true, true},
    // {"ppoll01", true, true}, // 2026-05-21: 四组合定向复测异常，RV 在 MASK_SIGNAL 子场景卡住，LA 在同子场景 kerneltrap
    // {"prctl01", true, true},
    // {"prctl02", true, true},
    // {"prctl03", true, true},
    // {"prctl04", true, true},
    // {"prctl05", true, true},
    // {"prctl06", true, true},
    // {"prctl06_execve", true, true},
    // {"prctl07", true, true},
    // {"prctl08", true, true},
    // {"prctl09", true, true},
    // {"prctl10", true, true},
    // {"pread02", true, true}, //爆了
    // {"pread02_64", true, true},
    // {"preadv01", true, true},
    // {"preadv01_64", true, true},
    // {"preadv02", true, true},
    // {"preadv02_64", true, true},
    // {"preadv03", true, true},
    // {"preadv03_64", true, true},
    // {"preadv201", true, true},
    // {"preadv201_64", true, true},
    // {"preadv202", true, true},
    // {"preadv202_64", true, true},
    // {"preadv203", true, true},
    // {"preadv203_64", true, true},
    // {"prepare_lvm.sh", true, true},
    // {"print_caps", true, true},
    // {"proc_sched_rt01", true, true},
    // {"proc01", true, true}, //pass
    // {"process_madvise01", true, true},
    // {"process_vm_readv02", true, true},
    // {"process_vm_readv03", true, true},
    // {"process_vm_writev02", true, true},
    // {"process_vm01", true, true},
    // {"profil01", true, true},
    // {"prot_hsymlinks", true, true},
    // {"pselect01", true, true}, // /bin/sh
    // {"pselect01_64", true, true},
    // {"pt_test", true, true},
    // {"ptem01", true, true},
    // {"pth_str01", true, true},
    // {"pth_str02", true, true},
    // {"pth_str03", true, true},
    // {"pthcli", true, true},
    // {"pthserv", true, true},
    // {"ptrace01", true, true},
    // {"ptrace02", true, true},
    // {"ptrace03", true, true},
    // {"ptrace04", true, true},
    // {"ptrace05", true, true},
    // {"ptrace06", true, true},
    // {"ptrace07", true, true},
    // {"ptrace08", true, true},
    // {"ptrace09", true, true},
    // {"ptrace10", true, true},
    // {"ptrace11", true, true},
    // {"pty01", true, true},
    // {"pty02", true, true},
    // {"pty03", true, true},
    // {"pty04", true, true},
    // {"pty05", true, true},
    // {"pty06", true, true},
    // {"pty07", true, true},
    // {"pwrite02", true, true},
    // {"pwrite02_64", true, true},
    // {"pwrite03", true, true},
    // {"pwrite03_64", true, true},
    // {"pwrite04", true, true},
    // {"pwrite04_64", true, true},
    // {"pwritev01", true, true},
    // {"pwritev01_64", true, true},
    // {"pwritev02", true, true},
    // {"pwritev02_64", true, true},
    // {"pwritev03", true, true},
    // {"pwritev03_64", true, true},
    // {"pwritev201", true, true},
    // {"pwritev201_64", true, true},
    // {"pwritev202", true, true},
    // {"pwritev202_64", true, true},
    // {"quota_remount_test01.sh", true, true},
    // {"quotactl01", true, true},
    // {"quotactl02", true, true},
    // {"quotactl03", true, true},
    // {"quotactl04", true, true},
    // {"quotactl05", true, true},
    // {"quotactl06", true, true},
    // {"quotactl07", true, true},
    // {"quotactl08", true, true},
    // {"quotactl09", true, true},
    // {"rcu_torture.sh", true, true},
    // {"read_all", true, true},
    // {"readahead01", true, true},
    // {"readahead02", true, true},
    // {"readdir01", true, true},
    // {"readdir21", true, true},
    // {"realpath01", true, true},
    // {"reboot01", true, true},
    // {"reboot02", true, true},
    // {"recv01", true, true},
    // {"recvfrom01", true, true}, // pass
    // {"recvmmsg01", true, true},
    // {"recvmsg01", true, true},
    // {"recvmsg02", true, true},
    // {"recvmsg03", true, true},
    // {"remap_file_pages01", true, true},
    // {"remap_file_pages02", true, true},
    // {"remove_password.sh", true, true},
    // {"removexattr01", true, true},
    // {"removexattr02", true, true},
    // {"rename01", true, true}, //bin/sh
    // {"rename03", true, true}, //bin/sh
    // {"rename04", true, true}, //bin/sh
    // {"rename05", true, true}, //bin/sh
    // {"rename06", true, true},
    // {"rename07", true, true},
    // {"rename08", true, true},
    // {"rename09", true, true},
    // {"rename10", true, true},
    // {"rename11", true, true},
    // {"rename12", true, true},
    // {"rename13", true, true},
    // {"rename14", true, true},
    // {"renameat01", true, true},
    // {"renameat201", true, true},
    // {"renameat202", true, true},
    // {"request_key01", true, true},
    // {"request_key02", true, true},
    // {"request_key03", true, true},
    // {"request_key04", true, true},
    // {"request_key05", true, true},
    // {"route4-rmmod", true, true},
    // {"route6-rmmod", true, true},
    // {"route-change-dst.sh", true, true},
    // {"route-change-gw.sh", true, true},
    // {"route-change-if.sh", true, true},
    // {"route-change-netlink", true, true},
    // {"route-change-netlink-dst.sh", true, true},
    // {"route-change-netlink-gw.sh", true, true},
    // {"route-change-netlink-if.sh", true, true},
    // {"route-lib.sh", true, true},
    // {"route-redirect.sh", true, true},
    // {"rt_sigaction01", true, true},
    // {"rt_sigaction02", true, true},
    // {"rt_sigaction03", true, true},
    // {"rt_sigprocmask01", true, true},
    // {"rt_sigprocmask02", true, true},
    // {"rt_sigqueueinfo01", true, true},
    // {"rt_sigsuspend01", true, true},
    // {"rtc01", true, true},
    // {"rtc02", true, true},
    // {"run_capbounds.sh", true, true},
    // {"run_cpuctl_latency_test.sh", true, true},
    // {"run_cpuctl_stress_test.sh", true, true},
    // {"run_cpuctl_test.sh", true, true},
    // {"run_cpuctl_test_fj.sh", true, true},
    // {"run_freezer.sh", true, true},
    // {"run_memctl_test.sh", true, true},
    // {"run_sched_cliserv.sh", true, true},
    // {"runpwtests_exclusive01.sh", true, true},
    // {"runpwtests_exclusive02.sh", true, true},
    // {"runpwtests_exclusive03.sh", true, true},
    // {"runpwtests_exclusive04.sh", true, true},
    // {"runpwtests_exclusive05.sh", true, true},
    // {"runpwtests01.sh", true, true},
    // {"runpwtests02.sh", true, true},
    // {"runpwtests03.sh", true, true},
    // {"runpwtests04.sh", true, true},
    // {"runpwtests05.sh", true, true},
    // {"runpwtests06.sh", true, true},
    // {"rwtest", true, true},
    // {"sbrk01", true, true}, // 爆了
    // {"sbrk02", true, true}, // pass
    // {"sbrk03", true, true}, // Arch需要是S390
    // {"sched_datafile", true, true},
    // {"sched_driver", true, true},
    // {"sched_get_priority_max01", true, true},
    // {"sched_get_priority_max02", true, true},
    // {"sched_get_priority_min01", true, true},
    // {"sched_get_priority_min02", true, true},
    // {"sched_getaffinity01", true, true}, // PASS
    // {"sched_getattr01", true, true},
    // {"sched_getattr02", true, true},
    // {"sched_getparam01", true, true},
    // {"sched_getparam03", true, true},
    // {"sched_getscheduler01", true, true},
    // {"sched_getscheduler02", true, true},
    // {"sched_rr_get_interval01", true, true},
    // {"sched_rr_get_interval02", true, true},
    // {"sched_rr_get_interval03", true, true},
    // {"sched_setaffinity01", true, true},
    // {"sched_setattr01", true, true},
    // {"sched_setparam01", true, true},
    // {"sched_setparam02", true, true},
    // {"sched_setparam03", true, true},
    // {"sched_setparam04", true, true},
    // {"sched_setparam05", true, true},
    // {"sched_setscheduler01", true, true},
    // {"sched_setscheduler02", true, true},
    // {"sched_setscheduler03", true, true},
    // {"sched_setscheduler04", true, true},
    // {"sched_stress.sh", true, true},
    // {"sched_tc0", true, true},
    // {"sched_tc1", true, true},
    // {"sched_tc2", true, true},
    // {"sched_tc3", true, true},
    // {"sched_tc4", true, true},
    // {"sched_tc5", true, true},
    // {"sched_tc6", true, true},
    // {"sched_yield01", true, true}, // pass
    // {"sctp_big_chunk", true, true},
    // {"sctp_ipsec.sh", true, true},
    // {"sctp_ipsec_vti.sh", true, true},
    // {"sctp01.sh", true, true},
    // {"select02", true, true},
    // {"select04", true, true},
    // {"sem_comm", true, true},
    // {"sem_nstest", true, true},
    // {"semctl01", true, true},
    // {"semctl02", true, true},
    // {"semctl03", true, true},
    // {"semctl04", true, true},
    // {"semctl05", true, true},
    // {"semctl06", true, true},
    // {"semctl07", true, true},
    // {"semctl08", true, true},
    // {"semctl09", true, true},
    // {"semget01", true, true},
    // {"semget02", true, true},
    // {"semget05", true, true},
    // {"semop01", true, true},
    // {"semop02", true, true},
    // {"semop03", true, true},
    // {"semop04", true, true},
    // {"semop05", true, true},
    // {"semtest_2ns", true, true},
    // {"send01", true, true},
    // {"send02", true, true},
    // {"sendfile01.sh", true, true},
    // {"sendfile02", true, true},
    // {"sendfile02_64", true, true},
    // {"sendfile03", true, true},
    // {"sendfile03_64", true, true},
    // {"sendfile04", true, true},
    // {"sendfile04_64", true, true},
    // {"sendfile05", true, true},
    // {"sendfile05_64", true, true},
    // {"sendfile06", true, true},
    // {"sendfile06_64", true, true},
    // {"sendfile07", true, true},
    // {"sendfile07_64", true, true},
    // {"sendfile08", true, true},
    // {"sendfile08_64", true, true},
    // {"sendfile09", true, true},
    // {"sendfile09_64", true, true},
    // {"sendmmsg01", true, true},
    // {"sendmmsg02", true, true},
    // {"sendmsg01", true, true},
    // {"sendmsg02", true, true},
    // {"sendmsg03", true, true},
    // {"sendto01", true, true}, // pass一部分
    // {"sendto02", true, true}, // pass
    // {"sendto03", true, true}, //.config
    // {"set_ipv4addr", true, true},
    // {"set_mempolicy01", true, true},
    // {"set_mempolicy02", true, true},
    // {"set_mempolicy03", true, true},
    // {"set_mempolicy04", true, true},
    // {"set_mempolicy05", true, true},
    // {"set_robust_list01", true, true},
    // {"set_thread_area01", true, true},
    // {"setdomainname01", true, true},
    // {"setdomainname02", true, true},
    // {"setdomainname03", true, true},
    // {"setfsgid01_16", true, true},
    // {"setfsgid02_16", true, true},
    // {"setfsgid03", true, true},
    // {"setfsgid03_16", true, true},
    // {"setfsuid01_16", true, true},
    // {"setfsuid02", true, true},
    // {"setfsuid02_16", true, true},
    // {"setfsuid03_16", true, true},
    // {"setfsuid04", true, true},
    // {"setfsuid04_16", true, true},
    // {"setgid01_16", true, true},
    // {"setgid02_16", true, true},
    // {"setgid03_16", true, true},
    // {"setgroups01", true, true},
    // {"setgroups01_16", true, true},
    // {"setgroups02", true, true},
    // {"setgroups02_16", true, true},
    // {"setgroups03", true, true},
    // {"setgroups03_16", true, true},
    // {"setgroups04", true, true},
    // {"setgroups04_16", true, true},
    // {"sethostname01", true, true},
    // {"sethostname02", true, true},
    // {"sethostname03", true, true},
    // {"setitimer01", true, true},
    // {"setitimer02", true, true},
    // {"setns01", true, true},
    // {"setns02", true, true},
    // {"setpgid01", true, true}, // pass
    // {"setpgid02", true, true}, // pass
    // {"setpgid03", true, true}, // 要完善sid逻辑, 而且现在退不出去, 先不修
    // {"setpgid03_child", true, true},
    // {"setpriority01", true, true},
    // {"setpriority02", true, true},
    // {"setregid01_16", true, true},
    // {"setregid02_16", true, true},
    // {"setregid03_16", true, true},
    // {"setregid04_16", true, true},
    // {"setresgid01_16", true, true},
    // {"setresgid02_16", true, true},
    // {"setresgid03_16", true, true},
    // {"setresgid04_16", true, true},
    // {"setreuid01_16", true, true},
    // {"setreuid02_16", true, true},
    // {"setreuid03_16", true, true},
    // {"setreuid04_16", true, true},
    // {"setreuid05_16", true, true},
    // {"setreuid06_16", true, true},
    // {"setreuid07_16", true, true},
    // {"setrlimit01", true, true},
    // {"setrlimit02", true, true},
    // {"setrlimit03", true, true},
    // {"setrlimit05", true, true},
    // {"setrlimit06", true, true},
    // {"setsockopt01", true, true}, // pass
    // {"setsockopt02", true, true},
    // {"setsockopt03", true, true}, // pass
    // {"setsockopt04", true, true},
    // {"setsockopt05", true, true}, //.config
    // {"setsockopt06", true, true}, //.config
    // {"setsockopt07", true, true}, //.config
    // {"setsockopt08", true, true},
    // {"setsockopt09", true, true}, //.config
    // {"setsockopt10", true, true}, //.config
    // {"settimeofday01", true, true},
    // {"settimeofday02", true, true},
    // {"setuid01_16", true, true},
    // {"setuid03_16", true, true},
    // {"setuid04", true, true},
    // {"setuid04_16", true, true},
    // {"setxattr01", true, true},
    // {"setxattr02", true, true},
    // {"setxattr03", true, true},
    // {"sgetmask01", true, true},
    // {"shell_pipe01.sh", true, true},
    // {"shm_comm", true, true},
    // {"shm_test", true, true},
    // {"shmat02", true, true},
    // {"shmat1", true, true},
    // {"shmctl01", true, true}, //卡死了
    // {"shmctl03", true, true}, //pass，但是这个似乎不能和别的连着跑
    // {"shmctl04", true, true}, //kernel doesn't support SHM_STAT_ANY
    // {"shmctl05", true, true}, // remap_file_pages未实现
    // {"shmctl06", true, true}, //test requires struct shmid64_ds to have the time_high fields
    // {"shmem_2nstest", true, true}, //看不懂
    // {"shmget02", true, true},
    // {"shmget03", true, true},
    // {"shmget04", true, true}, //爆了
    // {"shmget05", true, true}, //.config
    // {"shmget06", true, true}, //.config
    // {"shmnstest", true, true}, //pass
    // {"shmt02", true, true}, //pass 无summary
    // {"shmt03", true, true}, //pass 无summary
    // {"shmt04", true, true}, //pass 无summary
    // {"shmt05", true, true}, //pass 无summary
    // {"shmt06", true, true}, //pass 无summary
    // {"shmt07", true, true}, //pass 无summary
    // {"shmt08", true, true}, //pass 无summary
    // {"shmt09", true, true}, //sbrk 无summary
    // {"shmt10", true, true}, //pass 无summary
    // {"sigaction01", true, true},
    // {"sigaction02", true, true},
    // {"sigaltstack01", true, true},
    // {"sigaltstack02", true, true},
    // {"sighold02", true, true},
    // {"signal01", true, true},
    // {"signal02", true, true}, // pass 1 fail 2
    // {"signal06", true, true},
    // {"signalfd01", true, true},
    // {"signalfd4_01", true, true},
    // {"signalfd4_02", true, true},
    // {"sigpending02", true, true},
    // {"sigprocmask01", true, true},
    // {"sigrelse01", true, true},
    // {"sigsuspend01", true, true},
    // {"sigtimedwait01", true, true},
    // {"sigwait01", true, true},
    // {"sigwaitinfo01", true, true},
    // {"sit01.sh", true, true},
    // {"smack_common.sh", true, true},
    // {"smack_file_access.sh", true, true},
    // {"smack_notroot", true, true},
    // {"smack_set_ambient.sh", true, true},
    // {"smack_set_cipso.sh", true, true},
    // {"smack_set_current.sh", true, true},
    // {"smack_set_direct.sh", true, true},
    // {"smack_set_doi.sh", true, true},
    // {"smack_set_load.sh", true, true},
    // {"smack_set_netlabel.sh", true, true},
    // {"smack_set_onlycap.sh", true, true},
    // {"smack_set_socket_labels", true, true},
    // {"smt_smp_affinity.sh", true, true},
    // {"smt_smp_enabled.sh", true, true},
    // {"snd_seq01", true, true},
    // {"snd_timer01", true, true},
    // {"socketcall01", true, true},
    // {"socketcall02", true, true},
    // {"socketcall03", true, true},
    // {"socketpair01", true, true},
    // {"socketpair02", true, true},
    // {"sockioctl01", true, true},
    // {"splice01", true, true},
    // {"splice02", true, true},
    // {"splice03", true, true},
    // {"splice04", true, true},
    // {"splice05", true, true},
    // {"splice06", true, true},
    // {"splice08", true, true},
    // {"splice09", true, true},
    // {"squashfs01", true, true},
    // {"ssetmask01", true, true},
    // {"ssh-stress.sh", true, true},
    // {"stack_clash", true, true},
    // {"stack_space", true, true},
    // {"starvation", true, true},
    // {"stat02", true, true}, // pass
    // {"stat02_64", true, true}, // pass
    // {"statfs01", true, true}, ///dev/loop0
    // {"statfs01_64", true, true},
    // {"statfs03", true, true}, //爆了
    // {"statfs03_64", true, true},
    // {"statvfs01", true, true},
    // {"statvfs02", true, true}, //和别的不能一起跑
    // {"statx04", true, true}, //bin/sh
    // {"statx05", true, true},
    // {"statx06", true, true},
    // {"statx07", true, true},
    // {"statx08", true, true},
    // {"statx09", true, true}, //.config
    // {"statx10", true, true}, //bin/sh
    // {"statx11", true, true},
    // {"statx12", true, true},
    // {"stime01", true, true},
    // {"stime02", true, true},
    // {"stop_freeze_sleep_thaw_cont.sh", true, true},
    // {"stop_freeze_thaw_cont.sh", true, true},
    // {"stress", true, true},
    // {"string01", true, true}, //pass
    // {"support_numa", true, true},
    // {"swapoff01", true, true},
    // {"swapoff02", true, true},
    // {"swapon01", true, true},
    // {"swapon02", true, true},
    // {"swapon03", true, true},
    // {"swapping01", true, true},
    // {"symlinkat01", true, true}, // pass
    // {"sync_file_range01", true, true},
    // {"sync_file_range02", true, true},
    // {"sync01", true, true},
    // {"syncfs01", true, true},
    // {"sysconf01", true, true}, //没summary
    // {"sysctl01", true, true},
    // {"sysctl01.sh", true, true},
    // {"sysctl02.sh", true, true},
    // {"sysctl03", true, true},
    // {"sysctl04", true, true},
    // {"sysfs01", true, true},
    // {"sysfs02", true, true},
    // {"sysfs03", true, true},
    // {"sysfs04", true, true},
    // {"sysfs05", true, true},
    // {"sysinfo01", true, true},
    // {"sysinfo02", true, true},
    // {"sysinfo03", true, true},
    // {"syslog11", true, true},
    // {"syslog12", true, true},
    // {"tar_tests.sh", true, true},
    // {"tbio", true, true},
    // {"tc01.sh", true, true},
    // {"tcindex01", true, true},
    // {"tcp_cc_lib.sh", true, true},
    // {"tcp_fastopen_run.sh", true, true},
    // {"tcp_ipsec.sh", true, true},
    // {"tcp_ipsec_vti.sh", true, true},
    // {"tcp4-multi-diffip01", true, true},
    // {"tcp4-multi-diffip02", true, true},
    // {"tcp4-multi-diffip03", true, true},
    // {"tcp4-multi-diffip04", true, true},
    // {"tcp4-multi-diffip05", true, true},
    // {"tcp4-multi-diffip06", true, true},
    // {"tcp4-multi-diffip07", true, true},
    // {"tcp4-multi-diffip08", true, true},
    // {"tcp4-multi-diffip09", true, true},
    // {"tcp4-multi-diffip10", true, true},
    // {"tcp4-multi-diffip11", true, true},
    // {"tcp4-multi-diffip12", true, true},
    // {"tcp4-multi-diffip13", true, true},
    // {"tcp4-multi-diffip14", true, true},
    // {"tcp4-multi-diffnic01", true, true},
    // {"tcp4-multi-diffnic02", true, true},
    // {"tcp4-multi-diffnic03", true, true},
    // {"tcp4-multi-diffnic04", true, true},
    // {"tcp4-multi-diffnic05", true, true},
    // {"tcp4-multi-diffnic06", true, true},
    // {"tcp4-multi-diffnic07", true, true},
    // {"tcp4-multi-diffnic08", true, true},
    // {"tcp4-multi-diffnic09", true, true},
    // {"tcp4-multi-diffnic10", true, true},
    // {"tcp4-multi-diffnic11", true, true},
    // {"tcp4-multi-diffnic12", true, true},
    // {"tcp4-multi-diffnic13", true, true},
    // {"tcp4-multi-diffnic14", true, true},
    // {"tcp4-multi-diffport01", true, true},
    // {"tcp4-multi-diffport02", true, true},
    // {"tcp4-multi-diffport03", true, true},
    // {"tcp4-multi-diffport04", true, true},
    // {"tcp4-multi-diffport05", true, true},
    // {"tcp4-multi-diffport06", true, true},
    // {"tcp4-multi-diffport07", true, true},
    // {"tcp4-multi-diffport08", true, true},
    // {"tcp4-multi-diffport09", true, true},
    // {"tcp4-multi-diffport10", true, true},
    // {"tcp4-multi-diffport11", true, true},
    // {"tcp4-multi-diffport12", true, true},
    // {"tcp4-multi-diffport13", true, true},
    // {"tcp4-multi-diffport14", true, true},
    // {"tcp4-multi-sameport01", true, true},
    // {"tcp4-multi-sameport02", true, true},
    // {"tcp4-multi-sameport03", true, true},
    // {"tcp4-multi-sameport04", true, true},
    // {"tcp4-multi-sameport05", true, true},
    // {"tcp4-multi-sameport06", true, true},
    // {"tcp4-multi-sameport07", true, true},
    // {"tcp4-multi-sameport08", true, true},
    // {"tcp4-multi-sameport09", true, true},
    // {"tcp4-multi-sameport10", true, true},
    // {"tcp4-multi-sameport11", true, true},
    // {"tcp4-multi-sameport12", true, true},
    // {"tcp4-multi-sameport13", true, true},
    // {"tcp4-multi-sameport14", true, true},
    // {"tcp4-uni-basic01", true, true},
    // {"tcp4-uni-basic02", true, true},
    // {"tcp4-uni-basic03", true, true},
    // {"tcp4-uni-basic04", true, true},
    // {"tcp4-uni-basic05", true, true},
    // {"tcp4-uni-basic06", true, true},
    // {"tcp4-uni-basic07", true, true},
    // {"tcp4-uni-basic08", true, true},
    // {"tcp4-uni-basic09", true, true},
    // {"tcp4-uni-basic10", true, true},
    // {"tcp4-uni-basic11", true, true},
    // {"tcp4-uni-basic12", true, true},
    // {"tcp4-uni-basic13", true, true},
    // {"tcp4-uni-basic14", true, true},
    // {"tcp4-uni-dsackoff01", true, true},
    // {"tcp4-uni-dsackoff02", true, true},
    // {"tcp4-uni-dsackoff03", true, true},
    // {"tcp4-uni-dsackoff04", true, true},
    // {"tcp4-uni-dsackoff05", true, true},
    // {"tcp4-uni-dsackoff06", true, true},
    // {"tcp4-uni-dsackoff07", true, true},
    // {"tcp4-uni-dsackoff08", true, true},
    // {"tcp4-uni-dsackoff09", true, true},
    // {"tcp4-uni-dsackoff10", true, true},
    // {"tcp4-uni-dsackoff11", true, true},
    // {"tcp4-uni-dsackoff12", true, true},
    // {"tcp4-uni-dsackoff13", true, true},
    // {"tcp4-uni-dsackoff14", true, true},
    // {"tcp4-uni-pktlossdup01", true, true},
    // {"tcp4-uni-pktlossdup02", true, true},
    // {"tcp4-uni-pktlossdup03", true, true},
    // {"tcp4-uni-pktlossdup04", true, true},
    // {"tcp4-uni-pktlossdup05", true, true},
    // {"tcp4-uni-pktlossdup06", true, true},
    // {"tcp4-uni-pktlossdup07", true, true},
    // {"tcp4-uni-pktlossdup08", true, true},
    // {"tcp4-uni-pktlossdup09", true, true},
    // {"tcp4-uni-pktlossdup10", true, true},
    // {"tcp4-uni-pktlossdup11", true, true},
    // {"tcp4-uni-pktlossdup12", true, true},
    // {"tcp4-uni-pktlossdup13", true, true},
    // {"tcp4-uni-pktlossdup14", true, true},
    // {"tcp4-uni-sackoff01", true, true},
    // {"tcp4-uni-sackoff02", true, true},
    // {"tcp4-uni-sackoff03", true, true},
    // {"tcp4-uni-sackoff04", true, true},
    // {"tcp4-uni-sackoff05", true, true},
    // {"tcp4-uni-sackoff06", true, true},
    // {"tcp4-uni-sackoff07", true, true},
    // {"tcp4-uni-sackoff08", true, true},
    // {"tcp4-uni-sackoff09", true, true},
    // {"tcp4-uni-sackoff10", true, true},
    // {"tcp4-uni-sackoff11", true, true},
    // {"tcp4-uni-sackoff12", true, true},
    // {"tcp4-uni-sackoff13", true, true},
    // {"tcp4-uni-sackoff14", true, true},
    // {"tcp4-uni-smallsend01", true, true},
    // {"tcp4-uni-smallsend02", true, true},
    // {"tcp4-uni-smallsend03", true, true},
    // {"tcp4-uni-smallsend04", true, true},
    // {"tcp4-uni-smallsend05", true, true},
    // {"tcp4-uni-smallsend06", true, true},
    // {"tcp4-uni-smallsend07", true, true},
    // {"tcp4-uni-smallsend08", true, true},
    // {"tcp4-uni-smallsend09", true, true},
    // {"tcp4-uni-smallsend10", true, true},
    // {"tcp4-uni-smallsend11", true, true},
    // {"tcp4-uni-smallsend12", true, true},
    // {"tcp4-uni-smallsend13", true, true},
    // {"tcp4-uni-smallsend14", true, true},
    // {"tcp4-uni-tso01", true, true},
    // {"tcp4-uni-tso02", true, true},
    // {"tcp4-uni-tso03", true, true},
    // {"tcp4-uni-tso04", true, true},
    // {"tcp4-uni-tso05", true, true},
    // {"tcp4-uni-tso06", true, true},
    // {"tcp4-uni-tso07", true, true},
    // {"tcp4-uni-tso08", true, true},
    // {"tcp4-uni-tso09", true, true},
    // {"tcp4-uni-tso10", true, true},
    // {"tcp4-uni-tso11", true, true},
    // {"tcp4-uni-tso12", true, true},
    // {"tcp4-uni-tso13", true, true},
    // {"tcp4-uni-tso14", true, true},
    // {"tcp4-uni-winscale01", true, true},
    // {"tcp4-uni-winscale02", true, true},
    // {"tcp4-uni-winscale03", true, true},
    // {"tcp4-uni-winscale04", true, true},
    // {"tcp4-uni-winscale05", true, true},
    // {"tcp4-uni-winscale06", true, true},
    // {"tcp4-uni-winscale07", true, true},
    // {"tcp4-uni-winscale08", true, true},
    // {"tcp4-uni-winscale09", true, true},
    // {"tcp4-uni-winscale10", true, true},
    // {"tcp4-uni-winscale11", true, true},
    // {"tcp4-uni-winscale12", true, true},
    // {"tcp4-uni-winscale13", true, true},
    // {"tcp4-uni-winscale14", true, true},
    // {"tcp6-multi-diffip01", true, true},
    // {"tcp6-multi-diffip02", true, true},
    // {"tcp6-multi-diffip03", true, true},
    // {"tcp6-multi-diffip04", true, true},
    // {"tcp6-multi-diffip05", true, true},
    // {"tcp6-multi-diffip06", true, true},
    // {"tcp6-multi-diffip07", true, true},
    // {"tcp6-multi-diffip08", true, true},
    // {"tcp6-multi-diffip09", true, true},
    // {"tcp6-multi-diffip10", true, true},
    // {"tcp6-multi-diffip11", true, true},
    // {"tcp6-multi-diffip12", true, true},
    // {"tcp6-multi-diffip13", true, true},
    // {"tcp6-multi-diffip14", true, true},
    // {"tcp6-multi-diffnic01", true, true},
    // {"tcp6-multi-diffnic02", true, true},
    // {"tcp6-multi-diffnic03", true, true},
    // {"tcp6-multi-diffnic04", true, true},
    // {"tcp6-multi-diffnic05", true, true},
    // {"tcp6-multi-diffnic06", true, true},
    // {"tcp6-multi-diffnic07", true, true},
    // {"tcp6-multi-diffnic08", true, true},
    // {"tcp6-multi-diffnic09", true, true},
    // {"tcp6-multi-diffnic10", true, true},
    // {"tcp6-multi-diffnic11", true, true},
    // {"tcp6-multi-diffnic12", true, true},
    // {"tcp6-multi-diffnic13", true, true},
    // {"tcp6-multi-diffnic14", true, true},
    // {"tcp6-multi-diffport01", true, true},
    // {"tcp6-multi-diffport02", true, true},
    // {"tcp6-multi-diffport03", true, true},
    // {"tcp6-multi-diffport04", true, true},
    // {"tcp6-multi-diffport05", true, true},
    // {"tcp6-multi-diffport06", true, true},
    // {"tcp6-multi-diffport07", true, true},
    // {"tcp6-multi-diffport08", true, true},
    // {"tcp6-multi-diffport09", true, true},
    // {"tcp6-multi-diffport10", true, true},
    // {"tcp6-multi-diffport11", true, true},
    // {"tcp6-multi-diffport12", true, true},
    // {"tcp6-multi-diffport13", true, true},
    // {"tcp6-multi-diffport14", true, true},
    // {"tcp6-multi-sameport01", true, true},
    // {"tcp6-multi-sameport02", true, true},
    // {"tcp6-multi-sameport03", true, true},
    // {"tcp6-multi-sameport04", true, true},
    // {"tcp6-multi-sameport05", true, true},
    // {"tcp6-multi-sameport06", true, true},
    // {"tcp6-multi-sameport07", true, true},
    // {"tcp6-multi-sameport08", true, true},
    // {"tcp6-multi-sameport09", true, true},
    // {"tcp6-multi-sameport10", true, true},
    // {"tcp6-multi-sameport11", true, true},
    // {"tcp6-multi-sameport12", true, true},
    // {"tcp6-multi-sameport13", true, true},
    // {"tcp6-multi-sameport14", true, true},
    // {"tcp6-uni-basic01", true, true},
    // {"tcp6-uni-basic02", true, true},
    // {"tcp6-uni-basic03", true, true},
    // {"tcp6-uni-basic04", true, true},
    // {"tcp6-uni-basic05", true, true},
    // {"tcp6-uni-basic06", true, true},
    // {"tcp6-uni-basic07", true, true},
    // {"tcp6-uni-basic08", true, true},
    // {"tcp6-uni-basic09", true, true},
    // {"tcp6-uni-basic10", true, true},
    // {"tcp6-uni-basic11", true, true},
    // {"tcp6-uni-basic12", true, true},
    // {"tcp6-uni-basic13", true, true},
    // {"tcp6-uni-basic14", true, true},
    // {"tcp6-uni-dsackoff01", true, true},
    // {"tcp6-uni-dsackoff02", true, true},
    // {"tcp6-uni-dsackoff03", true, true},
    // {"tcp6-uni-dsackoff04", true, true},
    // {"tcp6-uni-dsackoff05", true, true},
    // {"tcp6-uni-dsackoff06", true, true},
    // {"tcp6-uni-dsackoff07", true, true},
    // {"tcp6-uni-dsackoff08", true, true},
    // {"tcp6-uni-dsackoff09", true, true},
    // {"tcp6-uni-dsackoff10", true, true},
    // {"tcp6-uni-dsackoff11", true, true},
    // {"tcp6-uni-dsackoff12", true, true},
    // {"tcp6-uni-dsackoff13", true, true},
    // {"tcp6-uni-dsackoff14", true, true},
    // {"tcp6-uni-pktlossdup01", true, true},
    // {"tcp6-uni-pktlossdup02", true, true},
    // {"tcp6-uni-pktlossdup03", true, true},
    // {"tcp6-uni-pktlossdup04", true, true},
    // {"tcp6-uni-pktlossdup05", true, true},
    // {"tcp6-uni-pktlossdup06", true, true},
    // {"tcp6-uni-pktlossdup07", true, true},
    // {"tcp6-uni-pktlossdup08", true, true},
    // {"tcp6-uni-pktlossdup09", true, true},
    // {"tcp6-uni-pktlossdup10", true, true},
    // {"tcp6-uni-pktlossdup11", true, true},
    // {"tcp6-uni-pktlossdup12", true, true},
    // {"tcp6-uni-pktlossdup13", true, true},
    // {"tcp6-uni-pktlossdup14", true, true},
    // {"tcp6-uni-sackoff01", true, true},
    // {"tcp6-uni-sackoff02", true, true},
    // {"tcp6-uni-sackoff03", true, true},
    // {"tcp6-uni-sackoff04", true, true},
    // {"tcp6-uni-sackoff05", true, true},
    // {"tcp6-uni-sackoff06", true, true},
    // {"tcp6-uni-sackoff07", true, true},
    // {"tcp6-uni-sackoff08", true, true},
    // {"tcp6-uni-sackoff09", true, true},
    // {"tcp6-uni-sackoff10", true, true},
    // {"tcp6-uni-sackoff11", true, true},
    // {"tcp6-uni-sackoff12", true, true},
    // {"tcp6-uni-sackoff13", true, true},
    // {"tcp6-uni-sackoff14", true, true},
    // {"tcp6-uni-smallsend01", true, true},
    // {"tcp6-uni-smallsend02", true, true},
    // {"tcp6-uni-smallsend03", true, true},
    // {"tcp6-uni-smallsend04", true, true},
    // {"tcp6-uni-smallsend05", true, true},
    // {"tcp6-uni-smallsend06", true, true},
    // {"tcp6-uni-smallsend07", true, true},
    // {"tcp6-uni-smallsend08", true, true},
    // {"tcp6-uni-smallsend09", true, true},
    // {"tcp6-uni-smallsend10", true, true},
    // {"tcp6-uni-smallsend11", true, true},
    // {"tcp6-uni-smallsend12", true, true},
    // {"tcp6-uni-smallsend13", true, true},
    // {"tcp6-uni-smallsend14", true, true},
    // {"tcp6-uni-tso01", true, true},
    // {"tcp6-uni-tso02", true, true},
    // {"tcp6-uni-tso03", true, true},
    // {"tcp6-uni-tso04", true, true},
    // {"tcp6-uni-tso05", true, true},
    // {"tcp6-uni-tso06", true, true},
    // {"tcp6-uni-tso07", true, true},
    // {"tcp6-uni-tso08", true, true},
    // {"tcp6-uni-tso09", true, true},
    // {"tcp6-uni-tso10", true, true},
    // {"tcp6-uni-tso11", true, true},
    // {"tcp6-uni-tso12", true, true},
    // {"tcp6-uni-tso13", true, true},
    // {"tcp6-uni-tso14", true, true},
    // {"tcp6-uni-winscale01", true, true},
    // {"tcp6-uni-winscale02", true, true},
    // {"tcp6-uni-winscale03", true, true},
    // {"tcp6-uni-winscale04", true, true},
    // {"tcp6-uni-winscale05", true, true},
    // {"tcp6-uni-winscale06", true, true},
    // {"tcp6-uni-winscale07", true, true},
    // {"tcp6-uni-winscale08", true, true},
    // {"tcp6-uni-winscale09", true, true},
    // {"tcp6-uni-winscale10", true, true},
    // {"tcp6-uni-winscale11", true, true},
    // {"tcp6-uni-winscale12", true, true},
    // {"tcp6-uni-winscale13", true, true},
    // {"tcp6-uni-winscale14", true, true},
    // {"tcpdump01.sh", true, true},
    // {"tee01", true, true},
    // {"tee02", true, true},
    // {"test.sh", true, true},
    // {"test_1_to_1_accept_close", true, true},
    // {"test_1_to_1_addrs", true, true},
    // {"test_1_to_1_connect", true, true},
    // {"test_1_to_1_connectx", true, true},
    // {"test_1_to_1_events", true, true},
    // {"test_1_to_1_initmsg_connect", true, true},
    // {"test_1_to_1_nonblock", true, true},
    // {"test_1_to_1_recvfrom", true, true},
    // {"test_1_to_1_recvmsg", true, true},
    // {"test_1_to_1_rtoinfo", true, true},
    // {"test_1_to_1_send", true, true},
    // {"test_1_to_1_sendmsg", true, true},
    // {"test_1_to_1_sendto", true, true},
    // {"test_1_to_1_shutdown", true, true},
    // {"test_1_to_1_socket_bind_listen", true, true},
    // {"test_1_to_1_sockopt", true, true},
    // {"test_1_to_1_threads", true, true},
    // {"test_assoc_abort", true, true},
    // {"test_assoc_shutdown", true, true},
    // {"test_autoclose", true, true},
    // {"test_basic", true, true},
    // {"test_basic_v6", true, true},
    // {"test_connect", true, true},
    // {"test_connectx", true, true},
    // {"test_controllers.sh", true, true},
    // {"test_fragments", true, true},
    // {"test_fragments_v6", true, true},
    // {"test_getname", true, true},
    // {"test_getname_v6", true, true},
    // {"test_inaddr_any", true, true},
    // {"test_inaddr_any_v6", true, true},
    // {"test_ioctl", true, true},
    // {"test_peeloff", true, true},
    // {"test_peeloff_v6", true, true},
    // {"test_recvmsg", true, true},
    // {"test_robind.sh", true, true},
    // {"test_sctp_sendrecvmsg", true, true},
    // {"test_sctp_sendrecvmsg_v6", true, true},
    // {"test_sockopt", true, true},
    // {"test_sockopt_v6", true, true},
    // {"test_tcp_style", true, true},
    // {"test_tcp_style_v6", true, true},
    // {"test_timetolive", true, true},
    // {"test_timetolive_v6", true, true},
    // {"testsf_c", true, true},
    // {"testsf_c6", true, true},
    // {"testsf_s", true, true},
    // {"testsf_s6", true, true},
    // {"tgkill01", true, true},
    // {"tgkill02", true, true},
    // {"tgkill03", true, true},
    // {"thp01", true, true},
    // {"thp02", true, true},
    // {"thp03", true, true},
    // {"thp04", true, true},
    // {"timed_forkbomb", true, true},
    // {"timens01", true, true}, //.config
    // {"timer_delete01", true, true},
    // {"timer_delete02", true, true},
    // {"timer_getoverrun01", true, true},
    // {"timer_gettime01", true, true},
    // {"timer_settime03", true, true},
    // {"timerfd_create01", true, true},
    // {"timerfd_gettime01", true, true},
    // {"timerfd_settime01", true, true},
    // {"timerfd_settime02", true, true},
    // {"timerfd01", true, true},
    // {"timerfd02", true, true},
    // {"timerfd04", true, true},
    // {"times01", true, true},
    // {"times03", true, true},
    // {"time-schedule", true, true},
    // {"tkill01", true, true},
    // {"tkill02", true, true},
    // {"tpci", true, true},
    // {"tpm_changeauth_tests.sh", true, true},
    // {"tpm_changeauth_tests_exp01.sh", true, true},
    // {"tpm_changeauth_tests_exp02.sh", true, true},
    // {"tpm_changeauth_tests_exp03.sh", true, true},
    // {"tpm_clear_tests.sh", true, true},
    // {"tpm_clear_tests_exp01.sh", true, true},
    // {"tpm_getpubek_tests.sh", true, true},
    // {"tpm_getpubek_tests_exp01.sh", true, true},
    // {"tpm_restrictpubek_tests.sh", true, true},
    // {"tpm_restrictpubek_tests_exp01.sh", true, true},
    // {"tpm_restrictpubek_tests_exp02.sh", true, true},
    // {"tpm_restrictpubek_tests_exp03.sh", true, true},
    // {"tpm_selftest_tests.sh", true, true},
    // {"tpm_takeownership_tests.sh", true, true},
    // {"tpm_takeownership_tests_exp01.sh", true, true},
    // {"tpm_version_tests.sh", true, true},
    // {"tpmtoken_import_tests.sh", true, true},
    // {"tpmtoken_import_tests_exp01.sh", true, true},
    // {"tpmtoken_import_tests_exp02.sh", true, true},
    // {"tpmtoken_import_tests_exp03.sh", true, true},
    // {"tpmtoken_import_tests_exp04.sh", true, true},
    // {"tpmtoken_import_tests_exp05.sh", true, true},
    // {"tpmtoken_import_tests_exp06.sh", true, true},
    // {"tpmtoken_import_tests_exp07.sh", true, true},
    // {"tpmtoken_import_tests_exp08.sh", true, true},
    // {"tpmtoken_init_tests.sh", true, true},
    // {"tpmtoken_init_tests_exp00.sh", true, true},
    // {"tpmtoken_init_tests_exp01.sh", true, true},
    // {"tpmtoken_init_tests_exp02.sh", true, true},
    // {"tpmtoken_init_tests_exp03.sh", true, true},
    // {"tpmtoken_objects_tests.sh", true, true},
    // {"tpmtoken_objects_tests_exp01.sh", true, true},
    // {"tpmtoken_protect_tests.sh", true, true},
    // {"tpmtoken_protect_tests_exp01.sh", true, true},
    // {"tpmtoken_protect_tests_exp02.sh", true, true},
    // {"tpmtoken_setpasswd_tests.sh", true, true},
    // {"tpmtoken_setpasswd_tests_exp01.sh", true, true},
    // {"tpmtoken_setpasswd_tests_exp02.sh", true, true},
    // {"tpmtoken_setpasswd_tests_exp03.sh", true, true},
    // {"tpmtoken_setpasswd_tests_exp04.sh", true, true},
    // {"trace_sched", true, true},
    // {"tracepath01.sh", true, true},
    // {"traceroute01.sh", true, true},
    // {"tst_ansi_color.sh", true, true},
    // {"tst_brk", true, true},
    // {"tst_brkm", true, true},
    // {"tst_cgctl", true, true},
    // {"tst_check_drivers", true, true},
    // {"tst_check_kconfigs", true, true},
    // {"tst_checkpoint", true, true},
    // {"tst_device", true, true},
    // {"tst_exit", true, true},
    // {"tst_fs_has_free", true, true},
    // {"tst_fsfreeze", true, true},
    // {"tst_get_free_pids", true, true},
    // {"tst_get_median", true, true},
    // {"tst_get_unused_port", true, true},
    // {"tst_getconf", true, true},
    // {"tst_hexdump", true, true},
    // {"tst_kvcmp", true, true},
    // {"tst_lockdown_enabled", true, true},
    // {"tst_ncpus", true, true},
    // {"tst_ncpus_conf", true, true},
    // {"tst_ncpus_max", true, true},
    // {"tst_net.sh", true, true},
    // {"tst_net_iface_prefix", true, true},
    // {"tst_net_ip_prefix", true, true},
    // {"tst_net_stress.sh", true, true},
    // {"tst_net_vars", true, true},
    // {"tst_ns_create", true, true},
    // {"tst_ns_exec", true, true},
    // {"tst_ns_ifmove", true, true},
    // {"tst_random", true, true},
    // {"tst_res", true, true},
    // {"tst_resm", true, true},
    // {"tst_rod", true, true},
    // {"tst_secureboot_enabled", true, true},
    // {"tst_security.sh", true, true},
    // {"tst_sleep", true, true},
    // {"tst_supported_fs", true, true},
    // {"tst_test.sh", true, true},
    // {"tst_timeout_kill", true, true},
    // {"uaccess", true, true},
    // {"udp_ipsec.sh", true, true},
    // {"udp_ipsec_vti.sh", true, true},
    // {"udp4-multi-diffip01", true, true},
    // {"udp4-multi-diffip02", true, true},
    // {"udp4-multi-diffip03", true, true},
    // {"udp4-multi-diffip04", true, true},
    // {"udp4-multi-diffip05", true, true},
    // {"udp4-multi-diffip06", true, true},
    // {"udp4-multi-diffip07", true, true},
    // {"udp4-multi-diffnic01", true, true},
    // {"udp4-multi-diffnic02", true, true},
    // {"udp4-multi-diffnic03", true, true},
    // {"udp4-multi-diffnic04", true, true},
    // {"udp4-multi-diffnic05", true, true},
    // {"udp4-multi-diffnic06", true, true},
    // {"udp4-multi-diffnic07", true, true},
    // {"udp4-multi-diffport01", true, true},
    // {"udp4-multi-diffport02", true, true},
    // {"udp4-multi-diffport03", true, true},
    // {"udp4-multi-diffport04", true, true},
    // {"udp4-multi-diffport05", true, true},
    // {"udp4-multi-diffport06", true, true},
    // {"udp4-multi-diffport07", true, true},
    // {"udp4-uni-basic01", true, true},
    // {"udp4-uni-basic02", true, true},
    // {"udp4-uni-basic03", true, true},
    // {"udp4-uni-basic04", true, true},
    // {"udp4-uni-basic05", true, true},
    // {"udp4-uni-basic06", true, true},
    // {"udp4-uni-basic07", true, true},
    // {"udp6-multi-diffip01", true, true},
    // {"udp6-multi-diffip02", true, true},
    // {"udp6-multi-diffip03", true, true},
    // {"udp6-multi-diffip04", true, true},
    // {"udp6-multi-diffip05", true, true},
    // {"udp6-multi-diffip06", true, true},
    // {"udp6-multi-diffip07", true, true},
    // {"udp6-multi-diffnic01", true, true},
    // {"udp6-multi-diffnic02", true, true},
    // {"udp6-multi-diffnic03", true, true},
    // {"udp6-multi-diffnic04", true, true},
    // {"udp6-multi-diffnic05", true, true},
    // {"udp6-multi-diffnic06", true, true},
    // {"udp6-multi-diffnic07", true, true},
    // {"udp6-multi-diffport01", true, true},
    // {"udp6-multi-diffport02", true, true},
    // {"udp6-multi-diffport03", true, true},
    // {"udp6-multi-diffport04", true, true},
    // {"udp6-multi-diffport05", true, true},
    // {"udp6-multi-diffport06", true, true},
    // {"udp6-multi-diffport07", true, true},
    // {"udp6-uni-basic01", true, true},
    // {"udp6-uni-basic02", true, true},
    // {"udp6-uni-basic03", true, true},
    // {"udp6-uni-basic04", true, true},
    // {"udp6-uni-basic05", true, true},
    // {"udp6-uni-basic06", true, true},
    // {"udp6-uni-basic07", true, true},
    // {"uevent01", true, true},
    // {"uevent02", true, true},
    // {"uevent03", true, true},
    // {"ulimit01", true, true}, // PASS
    // {"umask01", true, true},
    // {"umip_basic_test", true, true},
    // {"umount01", true, true},
    // {"umount02", true, true},
    // {"umount03", true, true},
    // {"umount2_01", true, true},
    // {"umount2_02", true, true},
    // {"uname04", true, true}, // 完全PASS
    // {"unshare01", true, true},
    // {"unshare01.sh", true, true},
    // {"unshare02", true, true},
    // {"unzip01.sh", true, true},
    // {"userfaultfd01", true, true},
    // {"userns01", true, true},
    // {"userns02", true, true},
    // {"userns03", true, true},
    // {"userns04", true, true},
    // {"userns05", true, true},
    // {"userns06", true, true},
    // {"userns06_capcheck", true, true},
    // {"userns07", true, true},
    // {"userns08", true, true},
    // {"ustat01", true, true},
    // {"ustat02", true, true},
    // {"utime01", true, true},
    // {"utime02", true, true},
    // {"utime03", true, true},
    // {"utime04", true, true},
    // {"utime05", true, true},
    // {"utime06", true, true},
    // {"utime07", true, true},
    // {"utimensat01", true, true},
    // {"utimes01", true, true},
    // {"utsname01", true, true},
    // {"utsname02", true, true},
    // {"utsname03", true, true},
    // {"utsname04", true, true},
    // {"verify_caps_exec", true, true},
    // {"vfork", true, true},
    // {"vfork_freeze.sh", true, true},
    // {"vfork01", true, true},
    // {"vfork02", true, true},
    // {"vhangup01", true, true},
    // {"vhangup02", true, true},
    // {"virt_lib.sh", true, true},
    // {"vlan01.sh", true, true},
    // {"vlan02.sh", true, true},
    // {"vlan03.sh", true, true},
    // {"vma01", true, true}, //pass 没有summary
    // {"vma02", true, true},
    // {"vma03", true, true},
    // {"vma04", true, true},
    // {"vma05.sh", true, true},
    // {"vma05_vdso", true, true},
    // {"vmsplice01", true, true},
    // {"vmsplice02", true, true},
    // {"vmsplice03", true, true},
    // {"vmsplice04", true, true},
    // {"vsock01", true, true},
    // {"vxlan01.sh", true, true},
    // {"vxlan02.sh", true, true},
    // {"vxlan03.sh", true, true},
    // {"vxlan04.sh", true, true},
    // {"wait01", true, true}, // PASS
    // {"wait02", true, true}, // PASS
    // {"wait401", true, true}, // PASS
    // {"wait402", true, true}, // PASS
    // {"wait403", true, true}, // PASS
    // {"waitid01", true, true},
    // {"waitid02", true, true},
    // {"waitid03", true, true},
    // {"waitid04", true, true},
    // {"waitid05", true, true},
    // {"waitid06", true, true},
    // {"waitid07", true, true},
    // {"waitid08", true, true},
    // {"waitid09", true, true},
    // {"waitid10", true, true},
    // {"waitid11", true, true},
    // {"waitpid08", true, true},
    // {"waitpid10", true, true},
    // {"waitpid11", true, true},
    // {"waitpid12", true, true},
    // {"waitpid13", true, true},
    // {"wc01.sh", true, true},
    // {"which01.sh", true, true},
    // {"wireguard_lib.sh", true, true},
    // {"wireguard01.sh", true, true},
    // {"wireguard02.sh", true, true},
    // {"wqueue01", true, true},
    // {"wqueue02", true, true},
    // {"wqueue03", true, true},
    // {"wqueue04", true, true},
    // {"wqueue05", true, true},
    // {"wqueue06", true, true},
    // {"wqueue07", true, true},
    // {"wqueue08", true, true},
    // {"wqueue09", true, true},
    // {"write_freezing.sh", true, true},
    // {"write06", true, true},
    // {"writetest", true, true},
    // {"writev01", true, true}, // 完全PASS
    // {"writev02", true, true},
    // {"writev03", true, true},
    // {"writev07", true, true},
    // {"zram_lib.sh", true, true},
    // {"zram01.sh", true, true},
    // {"zram02.sh", true, true},
    // {"zram03", true, true},

};
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
                const char *prefix = "/musl/usr/bin/";
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

int basic_musl_test(void)
{
    return basic_test(musl_dir);
}

int basic_glibc_test(void)
{
    return basic_test(glibc_dir);
}

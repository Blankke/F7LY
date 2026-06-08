
#include "user.hh"

extern char *libctest[][2];
extern char *libctest_dynamic_only[];

const char musl_dir[] = "/musl/";
const char glibc_dir[] = "/glibc/";

// LTP 测例结构体：{测例名字, RV+musl, RV+glibc, LA+musl, LA+glibc}
// 四个开关直接描述当前默认回归是否运行该组合；如果某测例只在特定 libc 或架构上跳过，
// 不要再额外写隐藏的 if/黑名单函数，直接在这里把对应组合置 false，并在表项注释原因。
struct ltp_testcase
{
    const char *name;
    bool test_riscv_musl;
    bool test_riscv_glibc;
    bool test_loongarch_musl;
    bool test_loongarch_glibc;
};

extern struct ltp_testcase ltp_testcases[];

extern char *git_testcases[][8];

#ifndef USER_TEST_HARNESS_DEBUG
#define USER_TEST_HARNESS_DEBUG 0
#endif

#if USER_TEST_HARNESS_DEBUG
#define HARNESS_PRINTF(...) printf(__VA_ARGS__)
#else
#define HARNESS_PRINTF(...) ((void)0)
#endif

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
    // 另外像 fcntl07 这类 LTP 用例会在子进程里通过 execlp(TCID, ...) 重新执行自己，
    // PATH 必须包含当前 testcase 目录，否则会被误判成内核/exec 失败。
    static char *musl_envp[] = {
        (char *)"PATH=/bin:/musl/ltp/testcases/bin",
        (char *)"LD_LIBRARY_PATH=/musl/lib",
#ifdef LOONGARCH
        // LoongArch QEMU 长回归里部分 LTP 用例会因为 fork/exec/shell 链路偏慢超过默认 30s。
        // 外层 make run 仍有 timeout 兜底，这里只避免 LTP 自身过早 SIGKILL。
        (char *)"LTP_TIMEOUT_MUL=10",
#endif
        NULL};
    static char *glibc_envp[] = {
        (char *)"PATH=/bin:/glibc/ltp/testcases/bin",
        (char *)"LD_LIBRARY_PATH=/glibc/lib",
#ifdef LOONGARCH
        (char *)"LTP_TIMEOUT_MUL=10",
#endif
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
        HARNESS_PRINTF("[FAIL] chdir(%s) 失败: %d\n", path, ret);
    }
    return ret;
}

static bool is_musl_test_root(const char *path)
{
    // 有些入口传 "/musl"，有些传 "/musl/"，这里统一视为同一个目录，
    // 避免测试标签被误打印成 glibc，影响后续排查。
    return strcmp(path, "/musl") == 0 || strcmp(path, musl_dir) == 0;
}

static bool is_libctest_dynamic_case_available(const char *case_name)
{
    // 镜像里的 entry-static.exe 与 entry-dynamic.exe 内置 case 集合不完全一致。
    // 下面两个 case 只存在于 static entry；传给 dynamic entry 会返回 255，属于调度错误而不是测例失败。
    return strcmp(case_name, "tls_align") != 0 &&
           strcmp(case_name, "pthread_cancel_sem_wait") != 0;
}

static bool ltp_case_enabled_for_current_combo(const ltp_testcase &testcase, bool is_musl)
{
#ifdef LOONGARCH
    return is_musl ? testcase.test_loongarch_musl : testcase.test_loongarch_glibc;
#else
    return is_musl ? testcase.test_riscv_musl : testcase.test_riscv_glibc;
#endif
}

static bool ltp_case_enabled_for_current_combo(const char *case_name, bool is_musl)
{
    for (int i = 0; ltp_testcases[i].name != NULL; ++i)
    {
        if (strcmp(ltp_testcases[i].name, case_name) == 0)
        {
            return ltp_case_enabled_for_current_combo(ltp_testcases[i], is_musl);
        }
    }
    // 定向调试时允许临时跑表外 case，避免新增测例前被调度层误拦截。
    return true;
}

int run_test(const char *path, char *argv[], char *envp[])
{
    char *default_argv[2] = {0};
    if (argv == 0)
    {
        default_argv[0] = (char *)path;
        argv = default_argv;
    }

    HARNESS_PRINTF("[RUN ] %s\n", path);
    int pid = fork();
    if (pid < 0)
    {
        HARNESS_PRINTF("[FAIL] %s: fork 失败\n", path);
        return -1;
    }
    else if (pid == 0)
    {
        // 每个测例都放进独立进程组里运行。
        // LTP 有些超时清理路径会对当前进程组发信号（例如 kill(0, SIGKILL)）。
        // 如果继续和回归主进程共用同一个 pgid，单个测例超时就可能把 init 一起打死，
        // 最后表现成“内核 panic: init exiting”，把整条长跑链路直接掐断。
        // 这里即便 setpgid 失败，也继续尝试 exec，让语义问题留给测例本身暴露。
        setpgid(0, 0);

        int exec_ret = execve(path, argv, envp);
        if (exec_ret < 0)
        {
            HARNESS_PRINTF("[FAIL] %s: execve 失败 ret=%d\n", path, exec_ret);
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
            HARNESS_PRINTF("[PASS] %s (exit=0)\n", path);
            return 0;
        }

        if (exited_normally)
        {
            HARNESS_PRINTF("[FAIL] %s (exit=%d, raw=0x%x)\n", path, result, child_exit_state);
        }
        else
        {
            HARNESS_PRINTF("[FAIL] %s (signal=%d, raw=0x%x)\n", path, -result, child_exit_state);
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
        HARNESS_PRINTF("[FAIL] mkdir(/bin) 失败: %d\n", mkdir_ret);
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

static int run_case_list_in_dir(const char *dir, const char *group_name, const char *const cases[], char *envp[], bool filter_ltp_combo = false, bool is_musl = false)
{
    if (dir == 0 || cases == 0)
    {
        HARNESS_PRINTF("[FAIL] %s: 参数为空\n", group_name ? group_name : "run_case_list_in_dir");
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
        if (filter_ltp_combo && !ltp_case_enabled_for_current_combo(cases[i], is_musl))
        {
            HARNESS_PRINTF("SKIP LTP CASE %s (disabled for current LTP combo)\n", cases[i]);
            continue;
        }
        argv[0] = (char *)cases[i];
        // 子集回归通常用于定向修复，必须稳定打印每个 case 的入口和返回值，
        // 避免非 debug 构建里 exec/等待失败被静默吞掉，导致日志只剩分组边界。
        printf("RUN CASE %s\n", cases[i]);
        int result = run_test(cases[i], argv, envp);
        printf("CASE RESULT %s: %d\n", cases[i], result);
        if (result != 0)
        {
            fail_count++;
        }
    }
    if (group_name != 0)
    {
        HARNESS_PRINTF("#### OS COMP TEST GROUP END %s (fail=%d) ####\n", group_name, fail_count);
        if (!USER_TEST_HARNESS_DEBUG)
        {
            printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
        }
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

static int expect_equal_bytes(const char *label, const char *actual, const char *expected, int len)
{
    for (int i = 0; i < len; ++i)
    {
        if (actual[i] != expected[i])
        {
            printf("[FAIL] %s: payload mismatch at %d, got=%d expected=%d\n",
                   label, i, actual[i], expected[i]);
            return -1;
        }
    }
    return 0;
}

static sockaddr_in loopback_addr(unsigned short port)
{
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = port;
    addr.sin_addr = 0x0100007f;
    for (int i = 0; i < 8; ++i)
    {
        addr.sin_zero[i] = 0;
    }
    return addr;
}

static constexpr int k_user_ipproto_tcp = 6;
static constexpr int k_user_tcp_maxseg = 2;
static constexpr int k_user_tcp_info = 11;

static int network_loopback_tcp_smoke(void)
{
    const char payload[] = "f7ly-loopback-tcp";
    char recv_buf[sizeof(payload)] = {};
    sockaddr_in bind_addr = loopback_addr(0);
    sockaddr_in server_addr;
    socklen_t server_len = sizeof(server_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0 || client_fd < 0)
    {
        printf("[FAIL] loopback tcp: socket failed server=%d client=%d\n", server_fd, client_fd);
        return -1;
    }

    if (bind(server_fd, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0 ||
        listen(server_fd, 4) < 0 ||
        getsockname(server_fd, (sockaddr *)&server_addr, &server_len) < 0)
    {
        printf("[FAIL] loopback tcp: bind/listen/getsockname failed\n");
        close(server_fd);
        close(client_fd);
        return -1;
    }

    server_addr.sin_addr = 0x0100007f;
    if (connect(client_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("[FAIL] loopback tcp: connect failed\n");
        close(server_fd);
        close(client_fd);
        return -1;
    }

    sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int accepted_fd = accept(server_fd, (sockaddr *)&peer_addr, &peer_len);
    if (accepted_fd < 0)
    {
        printf("[FAIL] loopback tcp: accept failed ret=%d\n", accepted_fd);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    int sent = write(client_fd, payload, sizeof(payload));
    int received = read(accepted_fd, recv_buf, sizeof(recv_buf));
    if (sent != (int)sizeof(payload) || received != (int)sizeof(payload) ||
        expect_equal_bytes("loopback tcp", recv_buf, payload, sizeof(payload)) != 0)
    {
        printf("[FAIL] loopback tcp: sent=%d received=%d\n", sent, received);
        close(accepted_fd);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    int maxseg = 0;
    socklen_t maxseg_len = sizeof(maxseg);
    if (getsockopt(client_fd, k_user_ipproto_tcp, k_user_tcp_maxseg, &maxseg, &maxseg_len) < 0 ||
        maxseg_len != sizeof(maxseg) || maxseg <= 0)
    {
        printf("[FAIL] loopback tcp: TCP_MAXSEG maxseg=%d len=%u\n", maxseg, maxseg_len);
        close(accepted_fd);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    unsigned char tcp_info[32] = {};
    socklen_t tcp_info_len = sizeof(tcp_info);
    if (getsockopt(client_fd, k_user_ipproto_tcp, k_user_tcp_info, tcp_info, &tcp_info_len) < 0 ||
        tcp_info_len == 0 || tcp_info[0] != 1)
    {
        printf("[FAIL] loopback tcp: TCP_INFO state=%u len=%u\n", tcp_info[0], tcp_info_len);
        close(accepted_fd);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    close(accepted_fd);
    close(server_fd);
    close(client_fd);
    printf("[PASS] loopback tcp payload\n");
    return 0;
}

static int network_loopback_udp_smoke(void)
{
    const char payload[] = "f7ly-loopback-udp";
    char recv_buf[sizeof(payload)] = {};
    sockaddr_in server_addr = loopback_addr(0);
    sockaddr_in actual_server_addr;
    sockaddr_in src_addr;
    socklen_t actual_server_len = sizeof(actual_server_addr);
    socklen_t src_len = sizeof(src_addr);

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0 || client_fd < 0)
    {
        printf("[FAIL] loopback udp: socket failed server=%d client=%d\n", server_fd, client_fd);
        return -1;
    }

    if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0 ||
        getsockname(server_fd, (sockaddr *)&actual_server_addr, &actual_server_len) < 0)
    {
        printf("[FAIL] loopback udp: bind/getsockname failed\n");
        close(server_fd);
        close(client_fd);
        return -1;
    }

    actual_server_addr.sin_addr = 0x0100007f;
    int sent = sendto(client_fd, payload, sizeof(payload), 0,
                      (sockaddr *)&actual_server_addr, sizeof(actual_server_addr));
    int received = recvfrom(server_fd, recv_buf, sizeof(recv_buf), 0,
                            (sockaddr *)&src_addr, &src_len);
    if (sent != (int)sizeof(payload) || received != (int)sizeof(payload) ||
        src_len != sizeof(sockaddr_in) || src_addr.sin_port == 0 ||
        expect_equal_bytes("loopback udp", recv_buf, payload, sizeof(payload)) != 0)
    {
        printf("[FAIL] loopback udp: sent=%d received=%d src_len=%u src_port=%u\n",
               sent, received, src_len, src_addr.sin_port);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    close(server_fd);
    close(client_fd);
    printf("[PASS] loopback udp payload\n");
    return 0;
}

static int network_loopback_udp_zero_datagram_smoke(void)
{
    char dummy = 'z';
    char recv_buf[1] = {};
    sockaddr_in server_addr = loopback_addr(0);
    sockaddr_in actual_server_addr;
    sockaddr_in src_addr;
    socklen_t actual_server_len = sizeof(actual_server_addr);
    socklen_t src_len = sizeof(src_addr);

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0 || client_fd < 0)
    {
        printf("[FAIL] loopback udp zero: socket failed server=%d client=%d\n", server_fd, client_fd);
        return -1;
    }

    if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0 ||
        getsockname(server_fd, (sockaddr *)&actual_server_addr, &actual_server_len) < 0)
    {
        printf("[FAIL] loopback udp zero: bind/getsockname failed\n");
        close(server_fd);
        close(client_fd);
        return -1;
    }

    actual_server_addr.sin_addr = 0x0100007f;
    int sent = sendto(client_fd, &dummy, 0, 0,
                      (sockaddr *)&actual_server_addr, sizeof(actual_server_addr));
    if (sent != 0)
    {
        printf("[FAIL] loopback udp zero: sendto returned %d\n", sent);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    // 零长度 UDP datagram 仍应唤醒接收端；用非阻塞接收避免回归失败时卡死。
    int received = recvfrom(server_fd, recv_buf, sizeof(recv_buf), MSG_DONTWAIT,
                            (sockaddr *)&src_addr, &src_len);
    if (sent != 0 || received != 0 || src_len != sizeof(sockaddr_in) || src_addr.sin_port == 0)
    {
        printf("[FAIL] loopback udp zero: sent=%d received=%d src_len=%u src_port=%u\n",
               sent, received, src_len, src_addr.sin_port);
        close(server_fd);
        close(client_fd);
        return -1;
    }

    close(server_fd);
    close(client_fd);
    printf("[PASS] loopback udp zero datagram\n");
    return 0;
}

int network_loopback_smoke(void)
{
    int fail_count = 0;
    printf("#### NETWORK LOOPBACK SMOKE START ####\n");
    if (network_loopback_tcp_smoke() != 0)
    {
        fail_count++;
    }
    if (network_loopback_udp_smoke() != 0)
    {
        fail_count++;
    }
    if (network_loopback_udp_zero_datagram_smoke() != 0)
    {
        fail_count++;
    }
    printf("#### NETWORK LOOPBACK SMOKE END fail=%d ####\n", fail_count);
    return fail_count == 0 ? 0 : -1;
}

int network_ltp_socket_subset(void)
{
    static const char *const socket_cases[] = {
        "socket01",
        "socket02",
        "accept01",
        "accept03",
        "accept4_01",
        "bind01",
        "listen01",
        "connect01",
        "sendto01",
        "recvfrom01",
        "recvmsg01",
        "socketpair02",
        NULL,
    };

    printf("#### NETWORK LTP SOCKET SUBSET START ####\n");
    init_env("/musl/");
    int fail_count = ltp_subset_test(true, socket_cases);
    printf("#### NETWORK LTP SOCKET SUBSET END fail=%d ####\n", fail_count);
    return fail_count == 0 ? 0 : -1;
}

int network_ltp_socket_abi_subset(void)
{
    static const char *const socket_abi_cases[] = {
        "recv01",
        "send01",
        "getsockname01",
        "getsockopt01",
        "setsockopt01",
        "sockioctl01",
        "socketpair01",
        NULL,
    };

    printf("#### NETWORK LTP SOCKET ABI SUBSET START ####\n");
    init_env("/musl/");
    int fail_count = ltp_subset_test(true, socket_abi_cases);
    printf("#### NETWORK LTP SOCKET ABI SUBSET END fail=%d ####\n", fail_count);
    return fail_count == 0 ? 0 : -1;
}

int network_ltp_socket_batch_subset(void)
{
    static const char *const socket_batch_cases[] = {
        "sendmmsg01",
        "send02",
        NULL,
    };

    printf("#### NETWORK LTP SOCKET BATCH SUBSET START ####\n");
    init_env("/musl/");
    int fail_count = ltp_subset_test(true, socket_batch_cases);
    printf("#### NETWORK LTP SOCKET BATCH SUBSET END fail=%d ####\n", fail_count);
    return fail_count == 0 ? 0 : -1;
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
    const char *group_name = is_musl_test_root(path) ? "iozone-musl" : "iozone-glibc";
    int fail_count = 0;

    // 将iozone testcode里面所有子测试全部搞出来一起测了。之前那个只有一个
    char *auto_measure[8] = {(char *)"iozone", (char *)"-a", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"4m", 0};
    char *write_read[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"0", (char *)"-i", (char *)"1", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *random_read[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"0", (char *)"-i", (char *)"2", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *read_backwards[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"0", (char *)"-i", (char *)"3", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *stride_read[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"0", (char *)"-i", (char *)"5", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *fwrite_fread[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"6", (char *)"-i", (char *)"7", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *pwrite_pread[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"9", (char *)"-i", (char *)"10", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};
    char *pwritev_preadv[16] = {(char *)"iozone", (char *)"-t", (char *)"4", (char *)"-i", (char *)"11", (char *)"-i", (char *)"12", (char *)"-r", (char *)"1k", (char *)"-s", (char *)"1m", 0};

    printf("#### OS COMP TEST GROUP START %s ####\n", group_name);
    printf("iozone automatic measurements\n");
    fail_count += run_test("iozone", auto_measure, 0) != 0;
    printf("iozone throughput write/read measurements\n");
    fail_count += run_test("iozone", write_read, 0) != 0;
    printf("iozone throughput random-read measurements\n");
    fail_count += run_test("iozone", random_read, 0) != 0;
    printf("iozone throughput read-backwards measurements\n");
    fail_count += run_test("iozone", read_backwards, 0) != 0;
    printf("iozone throughput stride-read measurements\n");
    fail_count += run_test("iozone", stride_read, 0) != 0;
    printf("iozone throughput fwrite/fread measurements\n");
    fail_count += run_test("iozone", fwrite_fread, 0) != 0;
    printf("iozone throughput pwrite/pread measurements\n");
    fail_count += run_test("iozone", pwrite_pread, 0) != 0;
    printf("iozone throughtput pwritev/preadv measurements\n");
    fail_count += run_test("iozone", pwritev_preadv, 0) != 0;
    printf("#### OS COMP TEST GROUP END %s ####\n", group_name);
    return fail_count == 0 ? 0 : -1;
}

int iperf_test(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }

    char **envp = ltp_envp(is_musl_test_root(path));
    char *bb_sh[8] = {0};
    bb_sh[0] = (char *)"busybox";
    bb_sh[1] = (char *)"sh";
    bb_sh[2] = (char *)"iperf_testcode.sh";
    run_test("busybox", bb_sh, envp);
    return 0;
}

int netperf_test(const char *path = musl_dir)
{
    if (change_dir_checked(path) != 0)
    {
        return -1;
    }

    char **envp = ltp_envp(is_musl_test_root(path));
    char *bb_sh[8] = {0};
    bb_sh[0] = (char *)"busybox";
    bb_sh[1] = (char *)"sh";
    bb_sh[2] = (char *)"netperf_testcode.sh";
    run_test("busybox", bb_sh, envp);
    return 0;
}

int libc_test(const char *path = musl_dir)
{
    [[maybe_unused]] int pid;

    if (change_dir_checked(path) != 0)
    {
        return -1;
    }

    char *argv[5] = {0};
    char **envp = ltp_envp(true);
    printf("#### OS COMP TEST GROUP START libctest-musl ####\n");

    // 线上评测以 libctest 自己的 runtest 输出（尤其是 Pass!）为准。
    // runtest 成功时在当前镜像里也会返回 1，不能用通用 harness 的退出码文案替代它自己的判定输出。
    argv[0] = "runtest.exe";
    argv[1] = "-w";
    argv[2] = "entry-static.exe";
    for (int i = 0; libctest[i][0] != NULL; i++)
    {
        argv[3] = libctest[i][0];
        run_test("runtest.exe", argv, envp);
    }

    argv[2] = "entry-dynamic.exe";
    for (int i = 0; libctest[i][0] != NULL; i++)
    {
        if (!is_libctest_dynamic_case_available(libctest[i][0]))
        {
            HARNESS_PRINTF("SKIP CASE entry-dynamic.exe %s (static-only)\n", libctest[i][0]);
            continue;
        }
        argv[3] = libctest[i][0];
        run_test("runtest.exe", argv, envp);
    }
    // 部分 libctest case 只存在于动态入口；单独调度，避免静态入口 255 误报污染回归。
    argv[2] = "entry-dynamic.exe";
    for (int i = 0; libctest_dynamic_only[i] != NULL; i++)
    {
        argv[3] = libctest_dynamic_only[i];
        run_test("runtest.exe", argv, envp);
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

    for (int i = 0; ltp_testcases[i].name != NULL; i++)
    {
        if (!ltp_case_enabled_for_current_combo(ltp_testcases[i], is_musl))
        {
            printf("SKIP LTP CASE %s (disabled for current LTP combo)\n", ltp_testcases[i].name);
            continue;
        }

        printf("RUN LTP CASE %s\n", ltp_testcases[i].name);
        bb_sh[0] = (char *)ltp_testcases[i].name;
        result = run_test(ltp_testcases[i].name, bb_sh, envp);
        // oscomp glibc judge 依赖 FAIL LTP CASE 收束当前 case；ret=0 是正常结束。
        printf("FAIL LTP CASE %s: %d\n", ltp_testcases[i].name, result);
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
        ltp_envp(is_musl),
        true,
        is_musl);
}

int priority_ltp_regression_riscv(void)
{
    static const char *const priority_cases[] = {
        "getpriority01",
        "getpriority02",
        "setpriority02",
        NULL,
    };

    printf("#### PRIORITY REGRESSION START riscv ####\n");
    init_env("/musl/");
    ltp_subset_test(true, priority_cases);
    ltp_subset_test(false, priority_cases);
    printf("#### PRIORITY REGRESSION END riscv ####\n");
    return 0;
}

int regression_suite_4d1444(void)
{
    printf("#### REGRESSION START commit-4d1444b-riscv ####\n");
    init_env("/musl/");
    basic_test("/musl/");
    basic_test("/glibc/");
    iozone_test("/musl");
    iozone_test("/glibc");
    libc_test("/musl/");
    lua_test("/musl/");
    lua_test("/glibc/");
    libcbench_test("/musl");
    libcbench_test("/glibc");
    ltp_test(true);
    ltp_test(false);
    busybox_test("/musl/");
    busybox_test("/glibc/");
    printf("#### REGRESSION END commit-4d1444b-riscv ####\n");
    return 0;
}

int bench_refine_suite(void)
{
    // 专用入口只保留 iozone 与 libcbench 四组合，缩短单轮验证时间。
    printf("#### BENCH REFINE SUITE START ####\n");
    init_env("/musl/");
    iozone_test("/musl");
    iozone_test("/glibc");
    libcbench_test("/musl");
    libcbench_test("/glibc");
    printf("#### BENCH REFINE SUITE END ####\n");
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
    {"pthread_cancel_points", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
    {"pthread_cancel", NULL},        // 2026-05-31: RV/LA static/dynamic 直跑 Pass
    {"pthread_cond", NULL}, // sig， fork高级用法
    {"pthread_tsd", NULL},  // sig， fork高级用法
    {"qsort", NULL},
    {"random", NULL},
    {"search_hsearch", NULL},
    {"search_insque", NULL},
    {"search_lsearch", NULL},
    {"search_tsearch", NULL},
    {"setjmp", NULL}, // 信号相关，爆了
    {"snprintf", NULL},
    {"socket", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass，依赖 SO_RCVTIMEO/SO_SNDTIMEO 兼容
    {"sscanf", NULL},
    {"sscanf_long", NULL}, // 龙芯会爆，riscv正常
    {"stat", NULL},        // fstat(fileno(f),&st)==0 failed: errnp = Bad file descriptor
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
    {"utime", NULL}, // sys_utimensat实现不正确
    {"wcsstr", NULL},
    {"wcstol", NULL},
    {"daemon_failure", NULL},
    {"dn_expand_empty", NULL},
    {"dn_expand_ptr_0", NULL},
    {"fflush_exit", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
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
    {"pthread_robust_detach", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
    {"pthread_cancel_sem_wait", NULL}, // sig， fork高级用法
    {"pthread_cond_smasher", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
    {"pthread_condattr_setclock", NULL}, // sig， fork高级用法
    {"pthread_exit_cancel", NULL},       // sig， fork高级用法
    {"pthread_once_deadlock", NULL},     // sig， fork高级用法
    {"pthread_rwlock_ebusy", NULL},      // sig， fork高级用法
    {"putenv_doublefree", NULL},
    {"regex_backref_0", NULL},
    {"regex_bracket_icase", NULL},
    {"regex_ere_backref", NULL},
    {"regex_escaped_high_byte", NULL},
    {"regex_negated_range", NULL},
    {"regexec_nosub", NULL},
    {"rewind_clear_error", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
    {"rlimit_open_files", NULL}, // 2026-05-31: RV/LA static/dynamic 直跑 Pass
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

char *libctest_dynamic_only[] = {
    (char *)"dlopen",         // 2026-05-31: static entry 返回 255，RV/LA dynamic 直跑 Pass
    (char *)"sem_init",       // 2026-05-31: static entry 返回 255，RV/LA dynamic 直跑 Pass
    (char *)"tls_init",       // 2026-05-31: static entry 返回 255，RV/LA dynamic 直跑 Pass
    (char *)"tls_local_exec", // 2026-05-31: static entry 返回 255，RV/LA dynamic 直跑 Pass
    NULL,
};

struct ltp_testcase ltp_testcases[] = {
    // 示例：{测例名字, RV+musl, RV+glibc, LA+musl, LA+glibc}
    // 约定：第一个 {NULL, false, false, false, false} 就是当前默认跑测例的结束标记。
    // 下面继续保留的注释清单只作为候选记录，想打开哪个测例就把它挪到结束标记前面。
    // 新开以前完全没跑过的测例时，优先按 tools/ltp/judge/ltp_rank.txt 的 total count 从高到低推进。
    {"fs_bind_cloneNS01.sh", true, true, true, true},
    {"fs_bind_cloneNS02.sh", true, true, true, true},
    {"fs_bind_cloneNS03.sh", true, true, true, true},
    {"fs_bind_cloneNS04.sh", true, true, true, true},
    {"fs_bind_cloneNS05.sh", true, true, true, true},
    {"fs_bind_cloneNS06.sh", true, true, true, true},
    {"fs_bind_cloneNS07.sh", true, true, true, true},
    {"fs_bind_lib.sh", true, true, true, true},
    {"fs_bind_move01.sh", true, true, true, true},
    {"fs_bind_move02.sh", true, true, true, true},
    {"fs_bind_move03.sh", true, true, true, true},
    {"fs_bind_move04.sh", true, true, true, true},
    {"fs_bind_move05.sh", true, true, true, true},
    {"fs_bind_move06.sh", true, true, true, true},
    {"fs_bind_move07.sh", true, true, true, true},
    {"fs_bind_move08.sh", true, true, true, true},
    {"fs_bind_move09.sh", true, true, true, true},
    {"fs_bind_move10.sh", true, true, true, true},
    {"fs_bind_move11.sh", true, true, true, true},
    {"fs_bind_move12.sh", true, true, true, true},
    {"fs_bind_move13.sh", true, true, true, true},
    {"fs_bind_move14.sh", true, true, true, true},
    {"fs_bind_move15.sh", true, true, true, true},
    {"fs_bind_move16.sh", true, true, true, true},
    {"fs_bind_move17.sh", true, true, true, true},
    {"fs_bind_move18.sh", true, true, true, true},
    {"fs_bind_move19.sh", true, true, true, true},
    {"fs_bind_move20.sh", true, true, true, true},
    {"fs_bind_move21.sh", true, true, true, true},
    {"fs_bind_move22.sh", true, true, true, true},
    {"fs_bind_rbind01.sh", true, true, true, true},
    {"fs_bind_rbind02.sh", true, true, true, true},
    {"fs_bind_rbind03.sh", true, true, true, true},
    {"fs_bind_rbind04.sh", true, true, true, true},
    {"fs_bind_rbind05.sh", true, true, true, true},
    {"fs_bind_rbind06.sh", true, true, true, true},
    {"fs_bind_rbind07.sh", true, true, true, true},
    {"fs_bind_rbind07-2.sh", true, true, true, true},
    {"fs_bind_rbind08.sh", true, true, true, true},
    {"fs_bind_rbind09.sh", true, true, true, true},
    {"fs_bind_rbind10.sh", true, true, true, true},
    {"fs_bind_rbind11.sh", true, true, true, true},
    {"fs_bind_rbind12.sh", true, true, true, true},
    {"fs_bind_rbind13.sh", true, true, true, true},
    {"fs_bind_rbind14.sh", true, true, true, true},
    {"fs_bind_rbind15.sh", true, true, true, true},
    {"fs_bind_rbind16.sh", true, true, true, true},
    {"fs_bind_rbind17.sh", true, true, true, true},
    {"fs_bind_rbind18.sh", true, true, true, true},
    {"fs_bind_rbind19.sh", true, true, true, true},
    {"fs_bind_rbind20.sh", true, true, true, true},
    {"fs_bind_rbind21.sh", true, true, true, true},
    {"fs_bind_rbind22.sh", true, true, true, true},
    {"fs_bind_rbind23.sh", true, true, true, true},
    {"fs_bind_rbind24.sh", true, true, true, true},
    {"fs_bind_rbind25.sh", true, true, true, true},
    {"fs_bind_rbind26.sh", true, true, true, true},
    {"fs_bind_rbind27.sh", true, true, true, true},
    {"fs_bind_rbind28.sh", true, true, true, true},
    {"fs_bind_rbind29.sh", true, true, true, true},
    {"fs_bind_rbind30.sh", true, true, true, true},
    {"fs_bind_rbind31.sh", true, true, true, true},
    {"fs_bind_rbind32.sh", true, true, true, true},
    {"fs_bind_rbind33.sh", true, true, true, true},
    {"fs_bind_rbind34.sh", true, true, true, true},
    {"fs_bind_rbind35.sh", true, true, true, true},
    {"fs_bind_rbind36.sh", true, true, true, true},
    {"fs_bind_rbind37.sh", true, true, true, true},
    {"fs_bind_rbind38.sh", true, true, true, true},
    {"fs_bind_rbind39.sh", true, true, true, true},
    {"fs_bind_regression.sh", true, true, true, true},
    {"fs_bind01.sh", true, true, true, true},
    {"fs_bind02.sh", true, true, true, true},
    {"fs_bind03.sh", true, true, true, true},
    {"fs_bind04.sh", true, true, true, true},
    {"fs_bind05.sh", true, true, true, true},
    {"fs_bind06.sh", true, true, true, true},
    {"fs_bind07.sh", true, true, true, true},
    {"fs_bind07-2.sh", true, true, true, true},
    {"fs_bind08.sh", true, true, true, true},
    {"fs_bind09.sh", true, true, true, true},
    {"fs_bind10.sh", true, true, true, true},
    {"fs_bind11.sh", true, true, true, true},
    {"fs_bind12.sh", true, true, true, true},
    {"fs_bind13.sh", true, true, true, true},
    {"fs_bind14.sh", true, true, true, true},
    {"fs_bind15.sh", true, true, true, true},
    {"fs_bind16.sh", true, true, true, true},
    {"fs_bind17.sh", true, true, true, true},
    {"fs_bind18.sh", true, true, true, true},
    {"fs_bind19.sh", true, true, true, true},
    {"fs_bind20.sh", true, true, true, true},
    {"fs_bind21.sh", true, true, true, true},
    {"fs_bind22.sh", true, true, true, true},
    {"fs_bind23.sh", true, true, true, true},
    {"fs_bind24.sh", true, true, true, true},
    {NULL,false,false,false,false},
    {"memfd_create01", true, true, true, true},
    {"splice01", true, true, true, true},
    {"splice02", true, true, true, true},
    {"splice03", true, true, true, true},
    {"splice04", true, true, true, true},
    {"splice05", true, true, true, true},
    {"splice06", true, true, true, true},
    {"splice07", true, true, true, true},
    {"splice08", true, true, true, true},
    {"splice09", true, true, true, true},
    {"epoll_ctl03", true, true, true, true},
    {"access01", true, true, true, true},
    {"access02", true, true, true, true},
    {"access03", true, true, true, false},
    {"access04", true, true, true, true},
    {"getpid01", true, true, true, true},
    {"pipe11", true, true, true, true},    // 2026-05-21: 四组合定向复测通过，total=70
    {"waitpid01", true, true, true, true}, // PASS
    {"timer_settime01", true, true, true, true},
    {"timer_settime02", true, true, true, true},
    {"clock_getres01", true, true, true, true},
    {"clock_gettime02", true, true, true, true}, // pass
    {"getitimer01", true, true, true, true},
    {"getitimer02", true, true, true, true},
    {"select01", true, true, true, true},
    {"select03", true, true, true, true},
    {"chmod01", true, true, true, true},
    {"chmod03", true, true, true, true}, // pass 4
    {"chmod06", true, true, true, true}, //   pass4 fail 5
    {"confstr01", true, true, true, true},
    {"creat01", true, true, true, true},         // passed   6
    {"creat06", true, true, true, true},         // pass
    {"posix_fadvise01", true, true, true, true}, // pass6
    {"posix_fadvise02", true, true, true, true}, // pass6
    {"posix_fadvise03", true, true, true, true},
    {"posix_fadvise01_64", true, true, true, true}, // pass6
    {"posix_fadvise02_64", true, true, true, true}, // pass6
    {"posix_fadvise03_64", true, true, true, true},
    {"signal03", true, true, true, true},
    {"signal04", true, true, true, true},
    {"signal05", true, true, true, true},
    {"add_key01", true, true, true, true},
    {"add_key02", true, true, true, true},
    {"add_key03", true, true, true, true},
    {"add_key04", true, true, true, true},
    {"dup01", true, true, true, true},            // 完全PASS
    {"dup02", true, true, true, true},            // 完全PASS
    {"dup03", true, true, true, true},            // 完全PASS
    {"dup04", true, true, true, true},            // 完全PASS
    {"dup05", true, true, true, true},            // pass
    {"dup06", true, true, true, true},            // 完全PASS
    {"dup07", true, true, true, true},            // 完全PASS
    {"dup201", true, true, true, true},           // 完全PASS
    {"dup202", true, true, true, true},           // 完全PASS
    {"dup203", true, true, true, true},           // pass
    {"dup204", true, true, true, true},           // 完全PASS
    {"dup205", true, true, true, true},           // 完全PASS
    {"dup206", true, true, true, true},           // 完全PASS
    {"epoll_create01", true, true, true, true},   // pass 2 skip 1
    {"epoll_create1_01", true, true, true, true}, // pass 1 skip 1
    {"fchdir01", true, true, true, true},         // 完全PASS
    {"fchdir02", true, true, true, true},         // 完全PASS
    {"fchmod01", true, true, true, true},         // pass
    {"fchmod03", true, true, true, true},         // pass
    {"fchmod04", true, true, true, true},         // pass
    {"fchmodat01", true, true, true, true},       // pass6
    {"fchmodat02", true, true, true, true},       // pass5 fail1
    {"fchown01", true, true, true, true},         // pass
    {"fchown02", true, true, true, true},         // pass 2 fail 1
    {"fchown03", true, true, true, true},         // pass
    {"fchown04", true, true, true, true},         // pass 2 fail 1
    {"fchown05", true, true, true, true},         // passed   6
    {"fcntl02", true, true, true, true},          // pass
    {"fcntl03", true, true, true, true},          // pass
    {"fcntl04", true, true, true, true},          // pass
    {"fcntl05", true, true, true, true},          // pass
    {"fcntl08", true, true, true, true},          // pass
    {"fcntl09", true, true, true, true},          // pass
    {"fcntl10", true, true, true, true},          // pass
    {"fcntl13", true, true, true, true},        // pass // la 会把用户态printf干爆
    {"fcntl15", true, true, true, true},         // RV+musl: LTP checkpoint/futex 同步稳定超时；glibc 和 LA 继续覆盖
    {"fcntl02_64", true, true, true, true},       // pass
    {"fcntl03_64", true, true, true, true},       // pass
    {"fcntl04_64", true, true, true, true},       // pass
    {"fcntl05_64", true, true, true, true},       // pass
    {"fcntl08_64", true, true, true, true},       // pass
    {"fcntl09_64", true, true, true, true},       // pass
    {"fcntl10_64", true, true, true, true},       // pass
    {"fcntl13_64", true, true, true, true},     // pass // la 会把用户态printf干爆
    {"fcntl15_64", true, true, true, true},      // RV+musl: 与 fcntl15 同源，同样会在 LTP checkpoint 同步阶段超时
    {"fstat02", true, true, true, true},          // pass 5 fail 1
    {"fstat03", true, true, false, false},        // pass2
    {"fstat02_64", true, true, true, true},       // pass 5 fail 1
    {"fstat03_64", true, true, false, false},     // pass2
    {"fstatfs02", true, true, true, true},      // pass 2
    {"fstatfs02_64", true, true, true, true},     // pass 2
    {"ftruncate01", true, true, true, true},      // pass 2
    {"ftruncate01_64", true, true, true, true},   // pass 2
    {"ftruncate03", true, true, true, true},      // pass 4
    {"faccessat01", true, true, true, true},      // 完全PASS
    {"faccessat02", true, true, true, true},      // 完全PASS
    {"faccessat201", false, true, true, true},    // RV+musl: LTP 内部 30s timeout；glibc 和 LA 继续覆盖
    {"setrlimit04", true, true, true, true},      // p1
    {"flock01", true, true, true, true},          // pass 3
    {"flock02", true, true, true, true},          // pass 3
    {"flock03", false, true, true, true},         // RV+musl: LTP checkpoint/futex 同步稳定超时；glibc 和 LA 继续覆盖
    {"flock04", true, true, true, true},          // pass5 fail1
    {"flock06", true, true, true, true},          // pass2 fail 2
    {"flistxattr01", true, true, true, true},     // pass 1
    {"flistxattr02", true, true, true, true},     // pass 2
    {"flistxattr03", true, true, true, true},     // pass 2
    {"fpathconf01", true, true, true, true},      // pass
    {"fsync02", true, true, true, true},        // pass
    {"fsync03", true, true, true, true},          // pass
    {"kill03", true, true, true, true},           // pass
    {"kill11", true, true, true, true},           // pass
    {"waitpid03", true, true, true, true},        // PASS
    {"waitpid04", true, true, true, true},        // PASS
    {"waitpid06", true, true, true, true},       // RV+musl: LTP checkpoint/futex 同步稳定超时；glibc 和 LA 继续覆盖
    {"waitpid07", true, true, true, true},       // RV+musl: LTP checkpoint/futex 同步稳定超时；glibc 和 LA 继续覆盖
    {"waitpid09", true, true, true, true},       // RV+musl: LTP checkpoint/futex 同步稳定超时；glibc 和 LA 继续覆盖
    {"getcwd01", true, true, true, true},         // pass
    {"getcwd02", true, true, true, true},         // 完全PASS
    {"getcwd03", true, true, true, true},       // pass
    {"getpgid01", true, true, true, true},        // PASS
    {"getpgid02", true, true, true, true},        // PASS
    {"getpid02", true, true, true, true},         // PASS
    {"getppid01", true, true, true, true},        // PASS
    {"getppid02", true, true, true, true},        // PASS
    {"getgid01", true, true, true, true},         // PASS
    {"getgid03", true, true, true, true},         // PASS
    {"getsid01", true, true, true, true},         // PASS
    {"getsid02", true, true, true, true},         // PASS
    {"getuid01", true, true, true, true},         // PASS
    {"getuid03", true, true, true, true},         // PASS
    {"setgid01", true, true, true, true},         // PASS
    {"setgid02", true, true, true, true},         // PASS
    {"setgid03", true, true, true, true},         // PASS
    {"setresgid01", true, true, true, true},      // 先等等
    {"setresgid02", true, true, true, true},      // 先等等
    {"setresgid03", true, true, true, true},      // 先等等
    {"setresgid04", true, true, true, true},      // 先等等
    {"setreuid01", true, true, true, true},       // PASS
    {"setreuid02", true, true, true, true},       // PASS
    {"setreuid03", true, true, true, true},       // PASS
    {"setreuid04", true, true, true, true},       // PASS
    {"setreuid05", true, true, true, true},       // PASS
    {"setreuid06", true, true, true, true},       // PASS
    {"setreuid07", true, true, true, true},       // p1 f2
    {"setregid01", true, true, true, true},       // PASS
    {"setregid02", true, true, true, true},       // PASS
    {"setregid03", true, true, true, true},       // PASS
    {"setregid04", true, true, true, true},       // PASS
    {"setegid01", true, true, true, true},        // PASS
    {"setegid02", true, true, true, true},        // PASS
    {"setfsgid01", true, true, true, true},       // p2 f1
    {"setfsgid02", true, true, true, true},       // PASS
    {"setfsuid01", true, true, true, true},       // PASS
    {"setfsuid03", true, true, true, true},       // PASS
    {"getpgrp01", true, true, true, true},        // PASS
    {"setpgrp01", true, true, true, true},        // PASS
    {"setpgrp02", true, true, true, true},        // PASS
    {"setuid01", true, true, true, true},         // PASS
    {"setuid03", true, true, true, true},         // PASS
    {"setresuid01", true, true, true, true},      // PASS
    {"setresuid02", true, true, true, true},      // PASS
    {"setresuid03", true, true, true, true},      // PASS
    {"setresuid04", true, true, true, true},      // p1 f2
    {"setresuid05", true, true, true, true},      // PASS
    {"getegid01", true, true, true, true},        // PASS
    {"getegid02", true, true, true, true},        // PASS
    {"geteuid01", true, true, true, true},        // PASS
    {"geteuid02", true, true, true, true},        // PASS
    {"clone01", true, true, true, true},          // pass
    {"clone03", true, true, true, true},          // pass
    {"clone06", true, true, true, true},          // pass
    {"clone302", true, true, true, true},         // p3 f5 s1
    {"getrandom01", true, true, true, true},      // pass
    {"getrandom02", true, true, true, true},      // 完全PASS
    {"getrandom03", true, true, true, true},      // 完全PASS
    {"getrandom04", true, true, true, true},      // 完全PASS
    {"getrandom05", true, true, true, true},      // pass
    {"getrlimit01", true, true, true, true},      // passed   16
    {"gettimeofday01", true, true, true, true},   // pass
    {"link02", true, true, true, true},           // pass
    {"link04", true, true, true, true},           // 2026-05-21: link 语义修正后四组合定向复测通过，total=14
    {"link08", true, true, true, true},           // 2026-05-21: link 权限/前缀错误码修正后四组合定向复测通过，total=4
    {"llseek01", true, true, true, true},         // pass
    {"llseek02", true, true, true, true},         // pass
    {"llseek03", true, true, true, true},         // pass
    {"lseek01", true, true, true, true},          // passed   4
    {"lseek02", false, true, true, true},         // RV+musl: LTP 内部 30s timeout；glibc 和 LA 继续覆盖
    {"lseek07", true, true, true, true},          // pass
    {"lstat01", true, true, true, true},
    {"lstat01_64", true, true, true, true},
    {"lstat02", true, true, true, true},
    {"lstat02_64", true, true, true, true},
    {"madvise01", true, true, true, true}, // pass
    {"madvise05", true, true, true, true},
    {"madvise10", true, true, true, true},
    {"mkdirat02", true, true, true, true}, // pass2fail2
    {"mkdir03", true, true, true, true},   // pass
    {"mknod02", true, true, true, true},
    {"mknod09", true, true, true, true},
    {"open01", true, true, true, true},        // pass
    {"open02", true, true, true, true},        // pass1 fail1
    {"open03", true, true, true, true},        // 完全PASS
    {"open04", true, true, true, true},        // 完全PASS
    {"open06", true, true, true, true},        // pass
    {"open07", true, true, true, true},        // pass
    {"open08", true, true, true, true},        // p4 f2
    {"open09", true, true, true, true},        // pass
    {"open10", false, true, false, true},      // musl: getgrgid() 先卡在统一镜像缺省组数据库；glibc 继续覆盖内核语义
    {"open11", true, true, true, true},        // 2026-05-21: 四组合定向复测通过，total=28
    {"openat01", true, true, true, true},      // pass
    {"pathconf01", true, true, true, true},    // pass
    {"pathconf02", true, true, true, true},  // musl: pathconf() 退化到 fpathconf(-1, name)，不会验证 path；glibc 继续覆盖
    {"personality01", true, true, true, true}, // 2026-05-21: personality(2) 补齐后四组合定向复测通过，total=18
    {"pipe01", true, true, true, true},        // 完全PASS
    {"pipe03", true, true, true, true},        // 完全PASS
    {"pipe06", true, true, true, true},        // 完全PASS
    {"pipe10", true, true, true, true},        // 完全PASS
    {"pipe12", true, true, true, true},        // pass
    {"pipe14", true, true, true, true},        // 完全PASS
    {"exit02", true, true, true, true},        // pass
    {"personality02", true, true, true, true},
    {"poll01", true, true, true, true},       // pass
    {"pread01", true, true, true, true},      // pass
    {"pread01_64", true, true, true, true},   // pass
    {"pselect02", true, true, true, true},    // pass
    {"pselect02_64", true, true, true, true}, // pass
    {"pselect03", true, true, true, true},    // pass
    {"pselect03_64", true, true, true, true}, // pass
    {"pwrite01", true, true, true, true},     // pass
    {"pwrite01_64", true, true, true, true},  // pass
    {"read01", true, true, true, true},       // 貌似可以PASS
    {"read02", true, true, true, true},       // pass
    {"read03", true, true, true, true},
    {"read04", true, true, true, true},        // 完全PASS
    {"readlink01", true, true, true, true},    // pass 2
    {"readlink03", true, true, false, true},   // LA+musl: bufsiz=0 被 musl 改成 dummy buffer；glibc 继续覆盖 EINVAL 路径
    {"readlinkat02", true, true, true, true}, // LA+musl: bufsiz=0 被 musl 改成 dummy buffer；glibc 继续覆盖 EINVAL 路径
    {"readv01", true, true, true, true},       // pass
    {"readv02", true, true, true, true},       // pass4 fail1
    {"rmdir01", true, true, true, true},       // pass
    {"rmdir02", true, true, true, true},       // pass
    {"rmdir03", true, true, true, true},       // 2026-05-21: sticky 目录删除权限语义修正后四组合定向复测通过，total=2
    {"shmat01", true, true, true, true},       // pass4
    {"shmat03", true, true, true, true},       // pass?
    {"shmat04", true, true, true, true},       // pass
    {"shmctl02", true, true, true, true},      // passed   16 fail 4
    {"shmctl07", true, true, true, true},      // pass
    {"shmctl08", true, true, true, true},      // pass
    {"shmdt01", true, true, true, true},       // pass 2
    {"shmdt02", true, true, true, true},       // pass
    {"stat01", true, true, true, true},        // passed   12
    {"stat03", true, true, true, true},        // pass4 fail2
    {"stat01_64", true, true, true, true},     // passed   12
    {"stat03_64", true, true, true, true},   // pass4 fail2
    {"statfs02", true, true, true, true},    // pass3fail3
    {"statfs02_64", true, true, true, true}, // pass3fail3
    {"statx01", true, true, true, true},       // pass8 fail2
    {"statx02", true, true, true, true},       // pass4 fail1
    {"statx03", true, true, true, true},       // pass6 fail1
    {"symlink02", true, true, true, true},     // pass
    {"symlink03", true, true, true, true},     // 2026-05-21: symlink 空路径/权限语义修正后四组合定向复测通过，total=0
    {"symlink04", true, true, true, true},     // pass
    {"syscall01", true, true, true, true},     // pass
    {"time01", true, true, true, true},        // pass
    {"truncate02", true, true, true, true},
    {"truncate02_64", true, true, true, true},
    {"truncate03", true, true, true, true},
    {"truncate03_64", true, true, true, true},
    {"uname01", true, true, true, true},      // 完全PASS
    {"uname02", true, true, true, true},      // 完全PASS
    {"unlink05", true, true, true, true},   // pass
    {"unlink07", true, true, true, true},   // pass
    {"unlink08", true, true, true, true},   // pass2fail2
    {"unlink09", true, true, true, true},   // pass
    {"unlinkat01", true, true, true, true}, // passed   7
    {"write01", true, true, true, true},    // 完全PASS
    {"write02", true, true, true, true},    // pass
    {"write03", true, true, true, true},    // 完全PASS
    {"write04", true, true, true, true},
    {"write05", true, true, true, true},  // passed   3
    {"writev05", true, true, true, true}, // 完全PASS
    {"writev06", true, true, true, true}, // 完全PASS
    {"execl01", true, true, true, true},    // PASS
    {"execle01", true, true, true, true},   // PASS
    {"execlp01", true, true, true, true},   // PASS
    {"execv01", true, true, true, true},    // PASS
    {"execve01", true, true, true, true},   // PASS
    {"execvp01", true, true, true, true},   // PASS
    {"gettid01", true, true, true, true}, // PASS
    {"set_tid_address01", true, true, true, true},
    {"getpriority01", true, true, true, true},
    {"getpriority02", true, true, true, true},
    {"setpriority02", true, true, false, false},
    {"alarm02", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"alarm03", true, false, true, false},           // 2026-05-27: musl Summary 通过；glibc 组合返回 TBROK，保持关闭。
    {"alarm07", true, false, true, false},           // 2026-05-27: musl Summary 通过；glibc 组合返回 TBROK，保持关闭。
    {"brk01", true, true, true, true},               // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"brk02", true, true, true, true},               // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chdir04", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chmod07", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chown01", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chown02", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chown03", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"chown05", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"clock_nanosleep04", true, true, true, true},   // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"close01", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"creat03", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"creat05", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"creat08", false, true, false, true},           // 2026-05-27: glibc TPASS；musl 组合返回 TBROK，保持关闭。
    {"dup207", true, true, true, true},              // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"dup3_02", true, true, true, true},             // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"epoll_create1_02", true, true, true, true},    // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"exit_group01", true, false, true, false},      // 2026-05-27: musl Summary 通过；glibc 组合返回 TBROK，保持关闭。
    {"fchmod02", true, true, true, true},            // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"fork01", true, false, true, false},            // 2026-05-27: musl Summary 通过；glibc 组合返回 TBROK，保持关闭。
    {"fork03", true, false, true, false},            // 2026-05-27: musl Summary 通过；glibc 组合返回 TBROK，保持关闭。
    {"fork04", true, false, true, false},           // 2026-05-27: LA+musl Summary 通过；其余组合不计分。
    {"futex_wait01", true, true, true, true},        // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"futex_wait04", true, true, true, true},        // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"futex_wait_bitset01", true, true, true, true}, // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"futex_wake01", true, true, true, true},        // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"getpagesize01", true, true, true, true},       // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    // {"gettimeofday02", true, true, true, true},      // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"link05", true, true, true, true},              // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"nanosleep04", true, true, true, true},         // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"nice01", true, true, true, true},              // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    // {"nice02", true, true, true, true},              // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"nice04", false, true, true, true},       // 2026-05-27: RV+musl Summary 失败，其余组合通过。
    {"pipe08", true, true, true, true},        // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"pipe2_01", true, true, true, true},      // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"signal02", true, true, true, true},      // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"times01", true, true, true, true},       // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"tkill01", true, true, true, true},       // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"uname04", true, true, true, true},       // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"wait01", true, true, true, true},        // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    {"wait402", true, true, true, true},       // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
   {"writev07", true, true, true, true},      // 2026-05-27: 扩展 probe 双架构验证，按 Summary/TPASS 规则开启通过组合。
    // {"stream01", false, true, false, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"stream02", false, true, false, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"stream03", false, true, false, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"stream04", false, true, false, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"stream05", true, true, true, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"abs01", true, true, true, true},       // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    {"stat02", true, true, true, true},        // 2026-05-26: 双架构双 libc Summary passed=2 failed=0 broken=0。
    {"stat02_64", true, true, true, true},     // 2026-05-26: 双架构双 libc Summary passed=2 failed=0 broken=0。
    {"string01", false, true, false, true},    // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"symlinkat01", true, true, true, true}, // 2026-05-26: 双架构 glibc TPASS；无 Summary，musl judge 不计分。
    // {"sysconf01", true, true, true, true},   // 2026-05-26: 双架构 glibc 有 TPASS/TCONF；无 Summary，musl judge 不计分。

    {"accept01", true, true, true, true},       // 2026-05-30: 四组合 passed 5 failed 0。
    // {"accept02", false, false, false, false},   // 2026-05-30: 未启用；MCAST_JOIN_GROUP 返回 ENOPROTOOPT，multicast socket option 未支持。
    {"accept03", true, true, true, true},       // 2026-05-30: 四组合 failed/broken 0；RV skipped 1，LA 组合全 pass。
    {"accept4_01", true, true, true, true},     // 2026-05-30: 四组合 passed 8 failed 0 skipped 1；__NR_socketcall 变体 TCONF。
    {"bind01", true, true, true, true},         // 2026-05-30: 四组合 passed 7 failed 0。
    {"bind02", true, true, true, true},         // 2026-05-30: 修复特权端口 EACCES 后四组合 passed 1 failed 0。
    {"bind03", true, true, true, true},         // 2026-05-30: 四组合 passed 3 failed 0。
    {"bind04", true, true, true, true},         // 2026-05-30: 四组合 passed 1 failed 0 skipped 1；seqpacket 变体 TCONF。
    // {"bind05", false, false, false, false},     // 2026-05-30: 未启用；AF_UNIX datagram bind 返回 EOPNOTSUPP，IPv4 UDP 子项能过但整体失败。
    // {"bind06", false, false, false, false},     // 2026-05-30: 未启用；依赖 USER_NS/NET_NS 与 AF_PACKET，当前为 TCONF。
    {"connect01", true, true, true, true},      // 2026-05-30: 四组合旧式 TPASS 7，无 Summary。
    // {"connect02", false, false, false, false},  // 2026-05-30: 未启用；IPV6_ADDRFORM 返回 ENOPROTOOPT，IPv6 地址转换 option 未支持。
    // {"getaddrinfo_01", false, false, false, false}, // 2026-05-30: 未启用；准备 /etc/hosts 时 write 返回 EBADF，测试直接 TBROK。
    {"gethostbyname_r01", false, true, false, true}, // 2026-05-30: glibc 通过；musl retval 不是 ERANGE，resolver/hosts 行为不一致。
    // {"gethostid01", false, false, false, false}, // 2026-05-30: 未启用；sethostid 或 /etc/hostid 环境不满足，glibc 下 ENOENT/TFAIL。
    {"gethostname01", true, true, true, true},   // 2026-05-27: 四组合 Summary/TPASS 通过，主机名读取基础路径可用。
    {"gethostname02", false, true, false, true}, // 2026-05-27: glibc TPASS；musl 组合 ENAMETOOLONG 语义不一致，保持关闭。
    {"getpeername01", true, true, true, true},   // 2026-05-30: 修复坏地址 errno 后四组合 passed 7 failed 0。
    {"getsockname01", true, true, true, true},   // 2026-05-30: 四组合 passed 6 failed 0。
    {"getsockopt01", true, true, true, true},    // 2026-05-30: 四组合 passed 9 failed 0。
    // {"getsockopt02", false, false, false, false}, // 2026-05-30: 未启用；SO_PEERCRED 返回 ENOPROTOOPT，peer credential 未支持。
    // {"listen01", true, true, true, true},        // 2026-05-30: 四组合旧式 TPASS 3，无 Summary。
    {"recv01", true, true, true, true},          // 2026-05-30: 四组合旧式 TPASS 5，无 Summary。
    // {"recvfrom01", true, true, true, true},      // 2026-05-30: 四组合旧式 TPASS 7，无 Summary。
    {"recvmsg01", true, true, true, true},       // 2026-05-30: 四组合 passed 10 failed 0。
    {"recvmsg02", true, true, true, true},       // 2026-05-30: 四组合 passed 1 failed 0。
    // {"recvmsg03", false, false, false, false},   // 2026-05-30: 未启用；RDS socket 不支持，当前为 TCONF。
    // {"recvmmsg01", false, false, false, false},  // 2026-05-30: 未启用；坏 msgvec 地址返回 -EFAULT，LTP raw syscall 判为 invalid retval -14。
    // {"send01", true, true, true, true},          // 2026-05-30: 四组合旧式 TPASS 6，无 Summary。
    {"send02", true, true, true, true},          // 2026-05-30: 四组合 passed 4 failed 0。
    {"sendmmsg01", true, true, true, true},      // 2026-05-30: 四组合 passed 4 failed 0。
    {"sendmmsg02", false, true, true, true},     // 2026-05-30: RV+musl libc wrapper 坏 msgvec 变体返回 EINVAL，其他组合 passed 4 failed 0。
    // {"sendmsg02", true, true, true, true},       // 2026-05-30: 四组合旧式 TPASS 1，无 Summary。
    // {"sendto01", true, true, true, true},        // 2026-05-30: 四组合旧式 TPASS 10，无 Summary。
    {"setsockopt01", true, true, true, true},    // 2026-05-30: 四组合 passed 8 failed 0。
    {"setsockopt03", true, true, true, true},    // 2026-05-30: 四组合 passed 1 failed 0 skipped 1；32 位 compat 变体 TCONF。
    {"setsockopt04", true, true, true, true},    // 2026-05-30: 接受 SO_SNDBUFFORCE 后四组合 passed 1 failed 0。
    {"sockioctl01", true, true, true, true},     // 2026-05-30: 四组合旧式 TPASS 8，无 Summary。
    {"socket01", true, true, true, true},        // 2026-05-30: 四组合 passed 9 failed 0。
    {"socket02", true, true, true, true},        // 2026-05-30: 四组合 passed 4 failed 0。
    {"socketpair01", true, true, true, true},    // 2026-05-30: 四组合 passed 10 failed 0。
    {"socketpair02", true, true, true, true},    // 2026-05-30: 四组合 passed 4 failed 0。
    {"mmap02", true, true, true, true},
    {"mmap05", true, true, true, true},        // pass1 但是panic关了一个
    {"mmap06", true, true, true, true},        // pass6 fail 2
    {"mmap08", true, true, true, true},        // pass
    {"mmap09", true, true, true, true},       // LA+glibc: LTP 内部 5min timeout；RV 和 LA+musl 继续覆盖
    {"mmap13", true, true, true, true},        // pass
    {"mmap15", true, true, true, true},        // pass
    {"mmap17", true, true, true, true},        // pass
    {"mmap19", true, true, true, true},        // pass
    {"mmap20", true, true, true, true},        // pass
    {"mmap001", true, true, true, true}, // pass.
    {"mmap01", true, true, true, true}, //bin/sh
    {"mmap03", false, true, false, true}, //无所谓，没summary
    {"mmap04", true, true, true, true},
    // {"mmap1", true, true, true, true}, // 2026-05-31: LA 默认回归在该 case 超过 8 分钟无输出，先关闭并待单测复核
    {"mmap10", false, true, false, true}, // 2026-05-31: RV/LA glibc 不再 SIGBUS；该 case 无 summary，musl 组合不计分
    // {"mmap11", true, true, true, true}, // 2026-05-31: 单跑 PASS；批量回归打印 test completed 后 wait 卡住，连续回归先关闭
    {"mmap12", true, true, true, true},
    {"mmap14", true, true, true, true},
    // {"mmap16", true, true, true, true}, // 2026-05-31: 镜像 PATH 缺 mkfs.ext4，LTP TCONF；按任务要求先不修环境
    {"mmap18", true, true, true, true},
    {"mmap2", true, true, true, true},
    // {"mmap3", true, true, true, true}, // 2026-05-31: RV+musl 40线程并发 mmap/write/munmap 触发 VMA 写回竞态与 kerneltrap，需内存管理大改
    // {"mmap-corruption01", true, true, true, true}, // 2026-05-31: 128MiB MAP_SHARED 失败后 SIGSEGV；需大页/非连续共享后端大改
    {"mmapstress01", true, true, true, true},
    {"mmapstress02", false, true, false, true}, // 2026-05-31: RV/LA glibc pass；musl TFAIL
    {"mmapstress03", false, true, false, true}, // 2026-05-31: RV/LA glibc TPASS；musl libc 的 sbrk(nonzero) 直接 ENOMEM，关闭
    {"mmapstress04", true, true, true, true},
    {"mmapstress05", false, true, false, true}, // 2026-05-31: RV/LA glibc pass；musl TFAIL
    // {"mmapstress06", true, true, true, true}, // 2026-05-31: 默认无参运行只打印 usage 后 TFAIL，需要 runtest 参数支持
    // {"mmapstress07", true, true, true, true}, // 2026-05-31: 默认无参运行只打印 usage 后 TFAIL，需要 runtest 参数支持
    // {"mmapstress08", true, true, true, true}, // 2026-05-31: LTP 标记仅适用于 IA-32/x86-64
    // {"mmapstress09", true, true, true, true}, // 2026-05-31: 默认无参运行只打印 usage 后 TFAIL，需要 runtest 参数支持
    // {"mmapstress10", true, true, true, true}, // 2026-05-31: 默认无参运行只打印 usage 后 TFAIL，需要 runtest 参数支持
    {"shm_comm", true, true, true, true}, // 2026-05-31: SysV IPC namespace 隔离，RV/LA musl+glibc TPASS
    // {NULL, false, false, false, false}, // 已验证并默认随回归运行的 mmap/shm 批次到这里结束
    // {"shm_test", true, true, true, true},//啥比
    {"shmat02", true, true, true, true},  //pass3
    // {"shmat1", true, true, true, true},//也是啥比
    {"shmctl01", true, true, true, true}, //pass 12
    {"shmctl03", true, true, true, true}, //pass4  TODO曾经有隐患待验证
    {"shmctl04", true, true, true, true}, // 2026-05-31: SHM_STAT_ANY 与 /proc/sysvipc/shm 四组合 passed 12 failed 0
    {"shmctl05", true, true, true, true}, // 2026-05-31: remap_file_pages 旧 ABI 兼容，四组合 passed 1 failed 0
    // {"shmctl06", true, true, true, true}, // 2026-05-31: 64位 RISC-V/LoongArch libc 未暴露 time_high 字段，LTP TCONF
    {"shmem_2nstest", true, true, true, true}, //pass 1
    {"shmget02", true, true, true, true}, // 2026-05-31: shmmax sysctl 写入与错误码四组合 passed 8 failed 0
    {"shmget03", true, true, true, true}, // 2026-05-31: /proc/sysvipc/shm 与 shmmni 四组合 passed 1 failed 0
    {"shmget04", true, true, true, true}, //passed   3
    {"shmget05", true, true, true, true}, //.config
    {"shmget06", true, true, true, true}, //.config
    {"shmnstest", true, true, true, true}, //pass 1
    {"shmt02", false, true, false, true}, //pass 无summary
    {"shmt03", true, true, true, true}, //pass 无summary
    {"shmt04", false, true, false, true}, //pass 无summary
    {"shmt05", false, true, false, true}, //pass 无summary
    {"shmt06", false, true, false, true}, //pass 无summary
    {"shmt07", false, true, false, true}, //pass 无summary
    {"shmt08", false, true, false, true}, //pass 无summary
    {"shmt09", false, true, false, true}, // 2026-05-31: glibc 无 summary；修正 brk 失败返回当前 break 后 RV/LA TPASS 4
    {"shmt10", false, true, false, true}, //pass 无summary
    // {NULL, false, false, false, false},  //待完成 2026.5.29 12:16分隔
    {"mkdir02", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"mkdir04", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"mkdir05", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"setsid01", true, true, true, true},          // 2026-05-28: RV+musl/glibc 均 TPASS，all misc tests passed。
    {"readlinkat01", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 12 failed 0。
    {"clock_adjtime01", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 9 failed 0。
    {"clock_adjtime02", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 6 failed 0。
    {"clock_gettime01", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 16 failed 0。
    {"clock_gettime03", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 24 failed 0。
    {"clock_gettime04", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 6 failed 0。
    {"clock_nanosleep01", true, true, true, true}, // 2026-05-28: RV+musl/glibc 均 passed 12 failed 0 skipped 2；libc wrapper BAD_TS_ADDR 变体被标记 TCONF。
    {"clock_nanosleep02", true, true, true, true}, // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    {"clock_nanosleep03", true, true, true, true}, // 2026-05-28: RV+musl/glibc 均 passed 2 failed 0。
    {"clock_settime01", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    {"clock_settime02", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 12 failed 0。
    {"clock_settime03", true, true, true, true},   // 2026-05-28: RV+musl/glibc 与 LA+glibc 均 passed 1 failed 0；LA+musl 全量回归中 rc=-9，仍有 “Main test process might have exit!”。
    {"clone02", false, true, false, true},         // 2026-05-28: RV+glibc passed 2 failed 0；musl 不启用。
    {"clone04", false, true, true, true},          // 2026-06-01: RV+musl 的 musl clone() 包装器在 NULL stack 场景进入 syscall 前 SIGSEGV，LTP 提示缺 musl 修复；内核错误路径由 RV+glibc/LA 覆盖。
    {"clone05", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"clone07", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"clone08", true, true, true, true},          // 2026-06-01: LA+musl 的 clone() 包装器在 CLONE_THREAD+NULL tls 组合进内核前返回 EINVAL；RV 与 LA+glibc 继续覆盖内核线程语义。
    {"clone09", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"clone301", true, true, true, true},          // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    {"clone303", true, true, true, true},          // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；cgroup v2 base controller 仍 TCONF。
    {"epoll_create02", false, true, true, true},   // 2026-06-01: RV+musl 的 libc epoll_create(size) 无法把 size 传给内核（仅落到 epoll_create1(0)），无内核可判别信号；RV+glibc/LA 覆盖内核语义。
    {"epoll_ctl01", true, true, true, true},       // 2026-05-28: RV+musl/glibc 均 passed 3。
    {"epoll_ctl02", true, true, true, true},       // 2026-05-28: RV+musl/glibc 均 passed 9。
    {"epoll_ctl04", true, true, true, true},       // 2026-05-28: RV+musl/glibc 均 passed 1。
    {"epoll_ctl05", true, true, true, true},       // 2026-05-28: RV+musl/glibc 均 passed 1。
    {"epoll_pwait01", true, true, true, true},     // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    {"epoll_pwait02", true, true, true, true},     // 2026-05-28: RV+musl/glibc 均 passed 2。
    {"epoll_pwait03", true, true, true, true},     // 2026-05-28: RV+musl/glibc 均 passed 14。
    {"epoll_pwait04", true, true, true, true},     // 2026-05-28: RV+musl/glibc 均 passed 2。
    {"epoll_pwait05", true, true, true, true},     // 2026-05-28: RV+musl/glibc 均 passed 3。
    {"epoll_wait01", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 3。
    {"epoll_wait02", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 7。
    {"epoll_wait03", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 5。
    {"epoll_wait04", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1。
    {"epoll_wait05", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1，Received EPOLLRDHUP。
    {"epoll_wait06", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 9。
    {"epoll_wait07", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 5。
    {"epoll-ltp", true, true, true, true},       // 2026-05-28: RV+glibc 与 LA+glibc 当前 TPASS 13857（epoll_create 33 + epoll_ctl 13824）；musl 仍未放开，todo。
    {"fallocate01", true, true, true, true},     // 2026-05-28: RV/LA + glibc 均 passed 4 failed 1；musl 不启用。
    {"fallocate02", false, true, false, true},     // 2026-05-28: RV+glibc passed 8 failed 0；musl 不启用。
    {"fallocate03", true, true, true, true},     // 2026-05-28: RV+glibc passed 8 failed 0；musl 不启用。
    {"fchdir03", true, true, true, true},          // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fchmod05", true, true, true, true},          // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fchmod06", true, true, true, true},          // 2026-05-28: RV/LA 四组合均 passed 0 failed 0 broken 1；mntpoint/dir/ 目录准备阶段 ENOENT。
    {"fchownat01", true, true, true, true},      // 2026-05-28: RV+glibc passed 5 failed 0；musl 不启用。
    {"fchownat02", false, true, false, true},      // 2026-05-28: RV+glibc passed 1 failed 0；musl 不启用。
    {"fcntl01", true, true, true, true},         // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl01_64", false, true, false, true},      // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl07", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    {"fcntl07_64", true, true, true, true},        // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    // {"fcntl11", true, true, true, true},           // 2026-05-28: RV+musl/glibc 均 ret=0，仅输出 TINFO，无 summary。
    // {"fcntl11_64", true, true, true, true},        // 2026-05-28: RV+musl/glibc 均 ret=0，仅输出 TINFO，无 summary。
    {"fcntl12", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl12_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl14", false, true, false, true},    /// pass
    {"fcntl14_64", false, true, false, true}, /// pass
    {"fcntl16", true, true, true, true},    // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl16_64", true, true, true, true}, // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl17", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl17_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl18", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 3 failed 0。
    {"fcntl18_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 3 failed 0。
    {"fcntl19", false, false, false, false},    // 能过,但是一个tpass都没有
    {"fcntl19_64", false, false, false, false}, // 能过,但是一个tpass都没有
    {"fcntl20", false, true, false, true},    // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl20_64", false, true, false, true}, // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl21", false, true, false, true},    // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl21_64", false, true, false, true}, // 2026-05-28: RV+musl/glibc 均 ret=0，但无 TPASS/TFAIL summary。
    {"fcntl22", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 2 failed 0。
    {"fcntl22_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 2 failed 0。
    {"fcntl23", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl23_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl24", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl24_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl25", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl25_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl26", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl26_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl27", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 2 failed 0。
    {"fcntl27_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 2 failed 0。
    {"fcntl29", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 3 failed 0。
    {"fcntl29_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 3 failed 0。
    {"fcntl30", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    {"fcntl30_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 4 failed 0。
    {"fcntl31", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 5 failed 0。
    {"fcntl31_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 5 failed 0。
    {"fcntl32", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 9 failed 0。
    {"fcntl32_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 9 failed 0。
    {"fcntl33", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    {"fcntl33_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    {"fcntl34", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl34_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 1 failed 0。
    {"fcntl35", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 2；unprivileged 初始 pipe 容量 4096，privileged 初始 pipe 容量 65536。
    {"fcntl35_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 2；unprivileged 初始 pipe 容量 4096，privileged 初始 pipe 容量 65536。
    {"fcntl36", true, true, true, true},      // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    {"fcntl36_64", true, true, true, true},   // 2026-05-28: RV+musl/glibc 均 passed 7 failed 0。
    
    {"atof01", true, true, true, true}, // PASS一部分 atof01.c:378: Test failed
    {"chmod05", true, true, true, true}, //   pass
    {"chown04", true, true, true, true},//failed   5
    {"clock_adjtime01", true, true, true, true}, //pass 9
    {"clock_adjtime02", true, true, true, true}, //pass 6
    {"clock_gettime01", true, true, true, true},//pass 16
    {"clock_gettime03", true, true, true, true},//pass 24
    {"clock_gettime04", true, true, true, true},//pass 6
    {"clock_nanosleep01", true, true, true, true}, //pass 12
    {"clock_nanosleep02", true, true, true, true},// pass 7
    {"clock_nanosleep03", true, true, true, true},//pass 2
    {"clock_settime01", true, true, true, true},//pass 4
    {"clock_settime02", true, true, true, true},//pass 12
    {"clock_settime03", true, true, true, true},//pass 1
    {"clone02", false, true, false, true}, //pass
    {"clone04", false, true, true, true}, // RV+musl 的 clone(NULL stack) 包装器问题已由前方权威条目关闭
    {"clone05", true, true, true, true},//pass
    {"clone07", true, true, true, true}, //pass
    {"clone08", true, true, true, true}, // LA+musl 的 CLONE_THREAD+NULL tls 包装器问题已由前方权威条目关闭
    {"clone09", true, true, true, true}, //pass
    {"clone301", true, true, true, true}, //pass
    {"close02", true, true, true, true}, //TFAIL: close(-1) expected EBADF: EPERM (1)
    {"creat04", true, true, true, true}, // pass
    {"execve02", true, true, true, true}, //TFAIL: execve_child shouldn't be executed
    {"execve03", true, true, true, true},//panic: kernel/mem/heap_memory_manager.cc:69: [hmm] alloc failed, size=0x8e638200000001cc, heap_total=0x000000000b73e000, heap_used=0x000000000002a62f, heap_cached=0x0000000000040000, chunks=2, coarse_free_pages=0x000000000000b6fe, coarse_max_block_bytes=0x0000000004000000
    {"execve04", true, true, true, true}, //TFAIL: execve_child shouldn't be executed
    {"execve05", true, true, true, true}, //pass
    {"execve06", true, true, true, true}, //pass
    {"exit01", true, true, true, true}, //pass
    {"execveat01", true, true, true, true}, //TCONF: syscall(281) __NR_execveat not supported on your arch
    {"execveat02", true, true, true, true}, //TCONF: syscall(281) __NR_execveat not supported on your arch
    {"faccessat202", true, true, true, true}, //pass
    {"fanotify02", true, true, true, true}, //TBROK: fanotify_mark(3, 0x1, 0x4800003b, ..., .) failed: ENOSYS (38)
    {"fanotify04", true, true, true, true}, //TBROK: fanotify_mark(3, 0x1, 0x4800003b, ..., .) failed: ENOSYS (38)
    {"fanotify08", true, true, true, true},
    {"fanotify11", true, true, true, true},//fail
    {"fanotify12", true, true, true, true}, //fail
    {"fanotify20", true, true, true, true},
    {"fdatasync01", false, true, false, true}, // pass
    {"fdatasync02", false, true, false, true}, // pass
    {"fgetxattr02", true, true, true, true}, //pass
    {"fgetxattr03", true, true, true, true},  //pass
    {"fork07", true, true, true, true},  //TFAIL: read() returns 1, expected 0
    {"fork08", true, true, true, true}, //pass
    {"fork09", false, true, false, true}, //pass
    {"fork10", true, true, true, true},  //pass
    {"fsconfig02", true, true, true, true},//TCONF: syscall(431) __NR_fsconfig not supported on your arch
    {"fsopen02", true, true, true, true},//TBROK: Could not stat loop device 8
    {"fstatat01", true, true, true, true}, //无summary
    {"getdents01", true, true, true, true}, // pass
    {"getdents02", true, true, true, true}, //pass
    {"getdomainname01", true, true, true, true}, // pass 1
    {"getegid01_16", true, true, true, true},// pass 1
    {"getegid02_16", true, true, true, true},// pass 1
    {"getgroups01", true, true, true, true},
    {"getgroups03", false, true, false, true},
    {"getresgid01", true, true, true, true},
    {"getresgid02", false, true, false, true},
    {"getresgid03", false, true, false, true},
    {"getresuid01", true, true, true, true},
    {"getresuid02", false, true, false, true},
    {"getresuid03", false, true, false, true},
    {"gettid02", true, true, true, true}, // PASS
    {"getxattr01", true, true, true, true}, //pass
    {"ioctl01", true, true, true, true},
    {"ioctl03", true, true, true, true},
    {"ioctl05", true, true, true, true},
    {"ioctl06", true, true, true, true},
    {"ioctl07", true, true, true, true},
    {"ioctl09", true, true, true, true},
    {"inotify01", true, true, true, true},
    {"inotify02", true, true, true, true},
    {"inotify04", true, true, true, true},
    {"inotify05", true, true, true, true},
    {"inotify06", true, true, true, true},
    {"inotify10", true, true, true, true},
    {"inotify11", true, true, true, true},
    {"inotify12", true, true, true, true},
    {"abort01", true, true, true, true},
    {"alarm05", true, true, true, true},
    {"alarm06", true, true, true, true},
    {"arping01.sh", false, false, false, false},  // 2026-05-30: 未纳入本轮复测；依赖 ARP/raw socket 与网络工具环境。
    {"capget01", true, true, true, true},
    {"capget02", true, true, true, true},
    {"capset01", true, true, true, true},
    {"chdir01", true, true, true, true},
    {"chroot03", true, true, true, true},
    {"close_range01", true, true, true, true},
    {"close_range02", true, true, true, true},
    {"copy_file_range01", true, true, true, true},
    {"diotest4", true, true, true, true},
    {"dup3_01", true, true, true, true},
    {"eventfd01", true, true, true, true},
    {"eventfd02", true, true, true, true},
    {"eventfd03", true, true, true, true},
    {"eventfd05", true, true, true, true},
    {"eventfd2_01", true, true, true, true},
    {"eventfd2_02", true, true, true, true},
    {"eventfd2_03", true, true, true, true},
    {"fanotify01", true, true, true, true},
    {"fanotify06", true, true, true, true},
    {"fanotify09", true, true, true, true},
    {"fanotify14", true, true, true, true},
    {"fanotify16", true, true, true, true},
    {"fanotify19", true, true, true, true},
    {"fgetxattr01", true, true, true, true},
    {"fremovexattr01", true, true, true, true},
    {"fremovexattr02", true, true, true, true},
    {"fsetxattr01", true, true, true, true},
    {"fspick02", true, true, true, true},
    {"get_robust_list01", true, true, true, true},
    {"inode01", true, true, true, true},
    {"ioctl_ns07", true, true, true, true},
    {"lchown01", true, true, true, true},
    {"linkat01", true, true, true, true},
    {"linkat02", true, true, true, true},
    {"memcmp01", true, true, true, true},
    {"memcpy01", true, true, true, true},
    {"memfd_create02", true, true, true, true},
    {"mkdir09", true, true, true, true},
    {"mknod01", true, true, true, true},
    {"mknod07", true, true, true, true},
    {"mlock01", true, true, true, true},
    {"msync01", true, true, true, true},
    {"msync02", true, true, true, true},
    {"munlock01", true, true, true, true},
    {"nanosleep02", true, true, true, true},
    {"newuname01", true, true, true, true},
    {"nextafter01", true, true, true, true},
    {"open_tree02", true, true, true, true},
    {"openat02", true, true, true, true},
    {"pipe04", true, true, true, true},
    {"prctl01", true, true, true, true},
    {"prctl02", true, true, true, true},
    {"pread02", true, true, true, true},
    {"pread02_64", true, true, true, true},
    {"pwrite02", true, true, true, true},
    {"pwrite02_64", true, true, true, true},
    {"readdir01", true, true, true, true},
    {"rename01", true, true, true, true},
    {"rename03", true, true, true, true},
    {"rename04", true, true, true, true},
    {"rename05", true, true, true, true},
    {"rename06", true, true, true, true},
    {"rename07", true, true, true, true},
    {"rename08", true, true, true, true},
    {"rename10", true, true, true, true},
    {"rename13", true, true, true, true},
    {"renameat01", true, true, true, true},
    {"rt_sigaction01", true, true, true, true},
    {"rt_sigaction02", true, true, true, true},
    {"rt_sigaction03", true, true, true, true},
    {"sched_getaffinity01", true, true, true, true},
    {"sched_setscheduler04", true, true, true, true},
    {"set_robust_list01", true, true, true, true},
    {"setgroups01", true, true, true, true},
    {"setgroups02", true, true, true, true},
    {"setitimer02", true, true, true, true},
    {"setpgid01", true, true, true, true},
    {"setrlimit02", true, true, true, true},
    {"settimeofday01", true, true, true, true},
    {"settimeofday02", true, true, true, true},
    {"setxattr01", true, true, true, true},
    {"sigaction01", true, true, true, true},
    {"sigaction02", true, true, true, true},
    {"sigaltstack01", true, true, true, true},
    {"sigaltstack02", true, true, true, true},
    {"signalfd01", true, true, true, true},
    {"sigpending02", true, true, true, true},
    {"sigprocmask01", true, true, true, true},
    {"statfs01", true, true, true, true},
    {"statfs01_64", true, true, true, true},
    {"statvfs01", true, true, true, true},
    {"statvfs02", true, true, true, true},
    {"symlink01", true, true, true, true},
    // {"sysinfo01", true, true, true, true},
    // {"sysinfo02", true, true, true, true},
    {"tgkill01", true, true, true, true},
    {"timerfd02", true, true, true, true},
    {"timerfd_create01", true, true, true, true},
    {"times03", true, true, true, true},
    {"tkill02", true, true, true, true},
    {"ulimit01", true, true, true, true},
    {"utime01", true, true, true, true},
    {"utime02", true, true, true, true},
    {"utime03", true, true, true, true},
    {"utime04", true, true, true, true},
    {"utimensat01", true, true, true, true},
    {"wait02", true, true, true, true},
    {"wait401", true, true, true, true},
    {"waitid01", true, true, true, true},
    {"waitid02", true, true, true, true},
    {"waitid03", true, true, true, true},
    {"waitid04", true, true, true, true},
    {"waitid05", true, true, true, true},
    {"waitid06", true, true, true, true},
    {"waitid11", true, true, true, true},
    {"waitpid08", true, true, true, true},
    {"writev01", true, true, true, true},
    {"writev02", true, true, true, true},
    {NULL, false, false, false, false},        // 已验证并默认随回归运行的测例，到这里结束

    // {"fcntl37", true, true, true, true},           // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；缺 capget syscall。
    // {"fcntl37_64", true, true, true, true},        // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；缺 capget syscall。
    // {"fcntl38", true, true, true, true},           // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；CONFIG_DNOTIFY 仍不满足。
    // {"fcntl38_64", true, true, true, true},        // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；CONFIG_DNOTIFY 仍不满足。
    // {"fcntl39", true, true, true, true},           // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；CONFIG_DNOTIFY 仍不满足。
    // {"fcntl39_64", true, true, true, true},        // 2026-05-28: 四组合均 passed 0 failed 0 skipped 1；CONFIG_DNOTIFY 仍不满足。
    // 2026-05-30: 网络 LTP 集中区。PASS 项按四组合开关启用；失败/TCONF/未复测项保留为注释。
    // 当前 ltp_test() 只能按单个可执行/脚本名运行，不能表达 runtest 中带参数的标签（如 ping601 -> ping01.sh -6）。
    // 因此网络脚本按实际入口集中登记；IPv4/IPv6、NFS/RPC/stress 等参数化变体在对应入口注释中说明。
    // {"sendmsg01", false, false, false, false},   // 2026-05-30: 未启用；AF_UNIX datagram server bind 返回 EOPNOTSUPP，测试 TBROK。
    // {"sendmsg03", false, false, false, false},   // 2026-05-30: 未启用；依赖 USER_NS/NET_NS 与 SOCK_RAW/IP_HDRINCL，当前为 TCONF。
    // {"sendto02", false, false, false, false},    // 2026-05-30: 未启用；SCTP protocol 不支持，当前为 TCONF。
    // {"sendto03", false, false, false, false},    // 2026-05-30: 未启用；依赖 USER_NS/NET_NS 与 AF_PACKET/PACKET_*，当前为 TCONF。
    // {"setsockopt02", false, false, false, false}, // 2026-05-30: 未启用；AF_PACKET/SOL_PACKET 不支持，当前为 TCONF。
    // {"setsockopt05", false, false, false, false}, // 2026-05-30: 未启用；依赖 USER_NS/NET_NS 与 SIOCSIFMTU/UFO 场景，当前为 TCONF。
    // {"setsockopt06", false, false, false, false}, // 2026-05-30: 未启用；依赖 AF_PACKET/TPACKET_V3 与 NET_NS，当前为 TCONF。
    // {"setsockopt07", false, false, false, false}, // 2026-05-30: 未启用；依赖 AF_PACKET/TPACKET_V3/PACKET_RESERVE，当前为 TCONF。
    // {"setsockopt08", false, false, false, false}, // 2026-05-30: 未启用；依赖 netfilter IPT_SO_SET_REPLACE，当前为 TCONF。
    // {"setsockopt09", false, false, false, false}, // 2026-05-30: 未启用；依赖 AF_PACKET/PACKET_RX_RING 与 NET_NS，当前为 TCONF。
    // {"vsock01", false, false, false, false},     // 2026-05-30: 未启用；AF_VSOCK/virtio-vsock/loopback vsock 当前未支持。
    // {"can_filter", false, false, false, false},  // 2026-05-30: 未启用；PF_CAN/CAN_RAW 协议族当前未支持。
    // {"can_rcv_own_msgs", false, false, false, false}, // 2026-05-30: 未启用；PF_CAN/CAN_RAW 协议族当前未支持。
    // {"can_bcm01", false, false, false, false},   // 2026-05-30: 未启用；PF_CAN/CAN_BCM 协议族当前未支持。
    // {"socketcall01", false, false, false, false}, // 2026-05-30: 未启用；RISC-V/LoongArch 无 __NR_socketcall 聚合 syscall。
    // {"socketcall02", false, false, false, false}, // 2026-05-30: 未启用；同 socketcall01，当前架构不提供 __NR_socketcall。
    // {"socketcall03", false, false, false, false}, // 2026-05-30: 未启用；同 socketcall01，bind/listen 聚合入口不存在。
    // {"setsockopt10", false, false, false, false}, // 2026-05-30: 未启用；依赖 TCP_ULP/TLS socket option，当前网络栈未支持。
    // {"in6_01", false, false, false, false},       // 2026-05-30: 未纳入本轮复测；IPv6 lib 测例，待 IPv6 基础语义单独验证。
    // {"in6_02", false, false, false, false},       // 2026-05-30: 未纳入本轮复测；IPv6 lib 测例，待 IPv6 基础语义单独验证。
    // {"asapi_01", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；IPv6 address selection API，待 IPv6 支持完善后再开。
    // {"asapi_02", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；IPv6 address selection API，待 IPv6 支持完善后再开。
    // {"asapi_03", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；IPv6 address selection API，待 IPv6 支持完善后再开。
    // {"bind_noport01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；脚本含 IPv4/IPv6 变体，依赖完整 LTP 网络脚本环境。
    // {"sendfile01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；网络版 sendfile 脚本依赖 rhost/ss/stat/diff 环境。
    // {"bbr01.sh", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；依赖 BBR、tc/iproute 与 netns 配置。
    // {"bbr02.sh", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；依赖 BBR、tc/iproute 与 netns 配置。
    // {"busy_poll01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 busy_poll 相关内核网络选项。
    // {"busy_poll02.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 busy_poll 相关内核网络选项。
    // {"busy_poll03.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 busy_poll 相关内核网络选项。
    // {"dccp01.sh", false, false, false, false},    // 2026-05-30: 未启用；DCCP protocol 当前未支持。
    // {"sctp01.sh", false, false, false, false},    // 2026-05-30: 未启用；SCTP protocol 当前未支持。
    // {"tcp_fastopen_run.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 TCP_FASTOPEN/sysctl，当前未支持。
    // {"vxlan01.sh", false, false, false, false},   // 2026-05-30: 未启用；VXLAN 虚拟网卡/隧道能力未支持。
    // {"vxlan02.sh", false, false, false, false},   // 2026-05-30: 未启用；VXLAN 虚拟网卡/隧道能力未支持。
    // {"vxlan03.sh", false, false, false, false},   // 2026-05-30: 未启用；VXLAN 虚拟网卡/隧道能力未支持。
    // {"vxlan04.sh", false, false, false, false},   // 2026-05-30: 未启用；VXLAN 虚拟网卡/隧道能力未支持。
    // {"vlan01.sh", false, false, false, false},    // 2026-05-30: 未启用；VLAN/虚拟链路设备未支持。
    // {"vlan02.sh", false, false, false, false},    // 2026-05-30: 未启用；VLAN/虚拟链路设备未支持。
    // {"vlan03.sh", false, false, false, false},    // 2026-05-30: 未启用；VLAN/虚拟链路设备未支持。
    // {"macvlan01.sh", false, false, false, false}, // 2026-05-30: 未启用；macvlan 虚拟链路设备未支持。
    // {"macvtap01.sh", false, false, false, false}, // 2026-05-30: 未启用；macvtap 虚拟链路设备未支持。
    // {"macsec01.sh", false, false, false, false},  // 2026-05-30: 未启用；MACsec 链路加密设备未支持。
    // {"macsec02.sh", false, false, false, false},  // 2026-05-30: 未启用；MACsec 链路加密设备未支持。
    // {"macsec03.sh", false, false, false, false},  // 2026-05-30: 未启用；MACsec 链路加密设备未支持。
    // {"ipvlan01.sh", false, false, false, false},  // 2026-05-30: 未启用；ipvlan 虚拟链路设备未支持。
    // {"gre01.sh", false, false, false, false},     // 2026-05-30: 未启用；GRE tunnel 能力未支持。
    // {"gre02.sh", false, false, false, false},     // 2026-05-30: 未启用；GRE tunnel 能力未支持。
    // {"fou01.sh", false, false, false, false},     // 2026-05-30: 未启用；FOU/GUE tunnel 能力未支持。
    // {"dctcp01.sh", false, false, false, false},   // 2026-05-30: 未启用；DCTCP/拥塞控制配置未支持。
    // {"geneve01.sh", false, false, false, false},  // 2026-05-30: 未启用；Geneve tunnel 能力未支持。
    // {"geneve02.sh", false, false, false, false},  // 2026-05-30: 未启用；Geneve tunnel 能力未支持。
    // {"sit01.sh", false, false, false, false},     // 2026-05-30: 未启用；SIT IPv6 tunnel 能力未支持。
    // {"mpls01.sh", false, false, false, false},    // 2026-05-30: 未启用；MPLS 路由/隧道能力未支持。
    // {"mpls02.sh", false, false, false, false},    // 2026-05-30: 未启用；MPLS 路由/隧道能力未支持。
    // {"mpls03.sh", false, false, false, false},    // 2026-05-30: 未启用；MPLS 路由/隧道能力未支持。
    // {"mpls04.sh", false, false, false, false},    // 2026-05-30: 未启用；MPLS 路由/隧道能力未支持。
    // {"fanout01", false, false, false, false},     // 2026-05-30: 未启用；AF_PACKET fanout 当前未支持。
    // {"wireguard01.sh", false, false, false, false}, // 2026-05-30: 未启用；WireGuard 虚拟设备/隧道能力未支持。
    // {"wireguard02.sh", false, false, false, false}, // 2026-05-30: 未启用；WireGuard 虚拟设备/隧道能力未支持。
    // {"ping01.sh", false, false, false, false},    // 2026-05-30: 未纳入本轮复测；依赖 ICMP/raw socket 与完整网络脚本环境。
    // {"ping02.sh", false, false, false, false},    // 2026-05-30: 未纳入本轮复测；依赖 ICMP/raw socket 与完整网络脚本环境。
    // {"tcpdump01.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 tcpdump/libpcap 与 AF_PACKET 抓包能力。
    // {"tracepath01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖路由/ICMP 与完整网络工具环境。
    // {"traceroute01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖路由/ICMP 与完整网络工具环境。
    // {"ipneigh01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 iproute/neighbour 表与 proc/net 语义。
    // {"iptables01.sh", false, false, false, false}, // 2026-05-30: 未启用；netfilter/iptables 当前未支持。
    // {"nft01.sh", false, false, false, false},     // 2026-05-30: 未启用；netfilter/nftables 当前未支持。
    // {"dhcpd_tests.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 dhcpd 服务和完整网络脚本环境。
    // {"dnsmasq_tests.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 dnsmasq 服务和完整网络脚本环境。
    // {"ip_tests.sh", false, false, false, false},  // 2026-05-30: 未纳入本轮复测；依赖 iproute 全功能与 netns。
    // {"netstat01.sh", false, false, false, false}, // 2026-05-30: 未纳入本轮复测；依赖 /proc/net 与网络统计语义。
    // {"ftp01.sh", false, false, false, false},     // 2026-05-30: 未纳入本轮复测；依赖 ftp 服务端/客户端脚本环境。
    // {"tc01.sh", false, false, false, false},      // 2026-05-30: 未纳入本轮复测；依赖 tc/qdisc 与 iproute 环境。
    // {"mc_cmds.sh", false, false, false, false},   // 2026-05-30: 未启用；multicast 测试脚本依赖组播协议与网络拓扑。
    // {"mc_commo.sh", false, false, false, false},  // 2026-05-30: 未启用；multicast 测试脚本依赖组播协议与网络拓扑。
    // {"mc_member.sh", false, false, false, false}, // 2026-05-30: 未启用；multicast 测试脚本依赖组播成员管理。
    // {"mc_opts.sh", false, false, false, false},   // 2026-05-30: 未启用；multicast socket options 当前未支持。
    // {"nfs01.sh", false, false, false, false},     // 2026-05-30: 未启用；net.nfs 参数化入口，依赖 NFS server/mount/rpcbind 与远端网络拓扑。
    // {"nfs02.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs03.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs04.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs05.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs06.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs07.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs08.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfs09.sh", false, false, false, false},     // 2026-05-30: 未启用；同 nfs01.sh，当前无完整 NFS 客户端/服务端测试环境。
    // {"nfslock01.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 NFS lockd/statd 与远端 NFS 锁测试环境。
    // {"nfsstat01.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 nfsstat/procfs NFS 统计与远端 NFS 服务。
    // {"fsx.sh", false, false, false, false},       // 2026-05-30: 未启用；net.nfs 的 fsx 入口依赖 NFS 挂载目标与远端服务。
    // {"rpc01.sh", false, false, false, false},     // 2026-05-30: 未启用；依赖 portmap/rpcbind 与 RPC 服务端脚本环境。
    // {"rpcinfo01.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 rpcinfo/rpcbind 与远端 RPC 服务发现。
    // {"rpc_test.sh", false, false, false, false},  // 2026-05-30: 未启用；net.rpc_tests/net.tirpc_tests 都需 -s/-c 参数，当前表结构无法表达。
    // {"ssh-stress.sh", false, false, false, false}, // 2026-05-30: 未启用；应用层 stress 依赖 sshd/远端主机与完整网络脚本环境。
    // {"dns-stress.sh", false, false, false, false}, // 2026-05-30: 未启用；应用层 stress 依赖 DNS 服务、远端主机与完整网络脚本环境。
    // {"http-stress.sh", false, false, false, false}, // 2026-05-30: 未启用；应用层 stress 依赖 HTTP 服务端/客户端脚本环境。
    // {"ftp-download-stress.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 FTP 服务端/远端主机与长时间网络 stress 环境。
    // {"ftp-upload-stress.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 FTP 服务端/远端主机与长时间网络 stress 环境。
    // {"broken_ip-version.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-ihl.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-fragment.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-plen.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-protcol.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-checksum.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-dstaddr.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 raw/AF_PACKET 构造坏包与 ns-tools，当前未支持。
    // {"broken_ip-nexthdr.sh", false, false, false, false}, // 2026-05-30: 未启用；IPv6 坏包 stress，依赖 IPv6/raw/AF_PACKET 与 ns-tools。
    // {"if4-addr-change.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ifconfig/iproute 修改网卡地址与 netns/多接口环境。
    // {"if-updown.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ip/ifconfig 上下线网卡与 netns/多接口环境。
    // {"if-addr-adddel.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ip/ifconfig 地址增删与 netns/多接口环境。
    // {"if-addr-addlarge.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖大量地址配置、iproute 与 netns/多接口环境。
    // {"if-route-adddel.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 route/iproute 路由增删与 netlink 路由语义。
    // {"if-route-addlarge.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖大量路由配置、iproute 与 netlink 路由语义。
    // {"if-mtu-change.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 SIOCSIFMTU/iproute 修改 MTU 与网卡设备模型。
    // {"route-change-dst.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖路由表/netlink route 与远端网络拓扑。
    // {"route-change-gw.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖路由表/netlink route 与远端网络拓扑。
    // {"route-change-if.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖路由表/netlink route 与多接口环境。
    // {"route-change-netlink-dst.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 NETLINK_ROUTE 与完整路由消息语义。
    // {"route-change-netlink-gw.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 NETLINK_ROUTE 与完整路由消息语义。
    // {"route-change-netlink-if.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 NETLINK_ROUTE 与多接口路由变更。
    // {"route-redirect.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ICMP redirect/路由协议行为与远端网络拓扑。
    // {"tcp_ipsec.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 IPsec/xfrm/setkey、加密算法与 netns/远端拓扑。
    // {"tcp_ipsec_vti.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 IPsec VTI tunnel/xfrm 与虚拟隧道设备。
    // {"udp_ipsec.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 IPsec/xfrm/setkey、加密算法与 netns/远端拓扑。
    // {"udp_ipsec_vti.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 IPsec VTI tunnel/xfrm 与虚拟隧道设备。
    // {"icmp-uni-basic.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ICMP/raw socket、IPsec 参数化脚本与远端网络拓扑。
    // {"icmp-uni-vti.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖 ICMP/raw socket、IPsec VTI tunnel 与远端网络拓扑。
    // {"sctp_ipsec.sh", false, false, false, false}, // 2026-05-30: 未启用；SCTP protocol 当前未支持，且依赖 IPsec/xfrm。
    // {"sctp_ipsec_vti.sh", false, false, false, false}, // 2026-05-30: 未启用；SCTP protocol 当前未支持，且依赖 IPsec VTI tunnel。
    // {"dccp_ipsec.sh", false, false, false, false}, // 2026-05-30: 未启用；DCCP protocol 当前未支持，且依赖 IPsec/xfrm。
    // {"dccp_ipsec_vti.sh", false, false, false, false}, // 2026-05-30: 未启用；DCCP protocol 当前未支持，且依赖 IPsec VTI tunnel。
    // {"mcast-group-single-socket.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖组播成员管理、IGMP/MLD 与 ns-tools。
    // {"mcast-group-multiple-socket.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖组播成员管理、IGMP/MLD 与 ns-tools。
    // {"mcast-group-same-group.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖组播成员管理、IGMP/MLD 与 ns-tools。
    // {"mcast-group-source-filter.sh", false, false, false, false}, // 2026-05-30: 未启用；依赖源过滤组播 socket option，当前未支持。
    // {"mcast-pktfld01.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 packet flood stress，依赖组播协议与远端拓扑。
    // {"mcast-pktfld02.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 packet flood stress，依赖组播协议与远端拓扑。
    // {"mcast-queryfld01.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"mcast-queryfld02.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"mcast-queryfld03.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"mcast-queryfld04.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"mcast-queryfld05.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"mcast-queryfld06.sh", false, false, false, false}, // 2026-05-30: 未启用；组播 query flood stress，依赖 IGMP/MLD/ns-tools。
    // {"sctp_big_chunk", false, false, false, false}, // 2026-05-30: 未启用；SCTP protocol 当前未支持。
    // {"test_1_to_1_accept_close", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_addrs", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_connect", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_connectx", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_events", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_initmsg_connect", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_nonblock", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_recvfrom", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_recvmsg", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_rtoinfo", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_send", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_sendmsg", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_sendto", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_shutdown", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_socket_bind_listen", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_sockopt", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_1_to_1_threads", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_assoc_abort", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_assoc_shutdown", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_autoclose", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_basic", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_basic_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_connect", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_connectx", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_fragments", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_fragments_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_getname", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_getname_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_inaddr_any", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_inaddr_any_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_peeloff", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_peeloff_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_recvmsg", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_sctp_sendrecvmsg", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_sctp_sendrecvmsg_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_sockopt", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_sockopt_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_tcp_style", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_tcp_style_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    // {"test_timetolive", false, false, false, false}, // 2026-05-30: 未启用；net.sctp 测例，SCTP protocol 当前未支持。
    // {"test_timetolive_v6", false, false, false, false}, // 2026-05-30: 未启用；net.sctp/IPv6 测例，SCTP 和 IPv6 完整语义当前未支持。
    
    // 以下补齐历史完整 LTP 清单，默认全部保持注释状态。
    
    // {"fallocate04", true, true, true, true},  //mkfs.ext2
    // {"fallocate05", true, true, true, true},  //mkfs.ext2
    // {"fallocate06", true, true, true, true}, //mkfs.ext2
    // {"fanotify03", true, true, true, true}, //mkfs.ext2
    // {"fanotify05", true, true, true, true},//mkfs.ext2
    // {"fanotify07", true, true, true, true},
    // {"fanotify10", true, true, true, true},
    // {"fanotify13", true, true, true, true},//mkfs.ext2
    // {"fanotify15", true, true, true, true},//TBROK: Could not stat loop device 19
    // {"fanotify17", true, true, true, true},//TBROK: Could not stat loop device 19
    // {"fanotify18", true, true, true, true},//TBROK: Could not stat loop device 19
    // {"fanotify21", true, true, true, true},
    // {"fanotify22", true, true, true, true},
    // {"fanotify23", true, true, true, true},
    // {"fdatasync03", true, true, true, true}, //TBROK: Could not stat loop device 20
    // {"fork05", true, true, true, true},
    // {"fork13", true, true, true, true}, //10m太长不跑
    // {"fork14", true, true, true, true}, // TCONF: mmap() fails too many times, so it's almost impossible to get a vm_area_struct sized 16TB
    // {"fsconfig01", true, true, true, true}, //TCONF: syscall(431) __NR_fsconfig not supported on your arch
    // {"fsconfig03", true, true, true, true},//TCONF: syscall(431) __NR_fsconfig not supported on your arch
    // {"fsetxattr02", true, true, true, true},
    // {"fsmount01", true, true, true, true},
    // {"fsmount02", true, true, true, true},
    // {"fsopen01", true, true, true, true}, //TBROK: Could not stat loop device 8
    // {"fspick01", true, true, true, true}, //TBROK: Could not stat loop device 8
    // {"fstatfs01", true, true, true, true}, ///dev/loop0
    // {"fstatfs01_64", true, true, true, true},
    // {"geteuid01_16", true, true, true, true},
    // {"geteuid02_16", true, true, true, true},
    // {"getgid01_16", true, true, true, true},
    // {"getgid03_16", true, true, true, true},
    // {"getgroups01_16", true, true, true, true},
    // {"getgroups03_16", true, true, true, true},
    // {"getresgid01_16", true, true, true, true},
    // {"getresgid02_16", true, true, true, true},
    // {"getresgid03_16", true, true, true, true},
    // {"getresuid01_16", true, true, true, true},
    // {"getresuid02_16", true, true, true, true},
    // {"getresuid03_16", true, true, true, true},
    // {"getuid01_16", true, true, true, true},
    // {"getuid03_16", true, true, true, true},
    // {"getxattr02", true, true, true, true}, //
    // {"getxattr03", true, true, true, true},
    // {"getxattr04", true, true, true, true},
    // {"getxattr05", true, true, true, true},
    // {"ioctl02", true, true, true, true},
    // {"ioctl04", true, true, true, true},
    // {"ioctl08", true, true, true, true},
    // {"inotify03", true, true, true, true},
    // {"inotify07", true, true, true, true},
    // {"inotify08", true, true, true, true},
    // {"inotify09", true, true, true, true},

    // {"acct01", true, true, true, true},
    // {"acct02", true, true, true, true},
    // {"acct02_helper", true, true, true, true},
    // {"acl1", true, true, true, true},
    // {"add_ipv6addr", true, true, true, true},
    // {"add_key05", true, true, true, true},
    // {"adjtimex01", true, true, true, true},
    // {"adjtimex02", true, true, true, true},
    // {"adjtimex03", true, true, true, true},
    // {"af_alg01", true, true, true, true},
    // {"af_alg02", true, true, true, true},
    // {"af_alg03", true, true, true, true},
    // {"af_alg04", true, true, true, true},
    // {"af_alg05", true, true, true, true},
    // {"af_alg06", true, true, true, true},
    // {"af_alg07", true, true, true, true},
    // {"aio01", true, true, true, true},
    // {"aio02", true, true, true, true},
    // {"aiocp", true, true, true, true},
    // {"aiodio_append", true, true, true, true},
    // {"aiodio_sparse", true, true, true, true},
    // {"aio-stress", true, true, true, true},
    // {"ar01.sh", true, true, true, true},
    // {"arch_prctl01", true, true, true, true},
    // {"ask_password.sh", true, true, true, true},
    // {"aslr01", true, true, true, true},
    // {"assign_password.sh", true, true, true, true},
    // {"autogroup01", true, true, true, true},
    // {"binfmt_misc_lib.sh", true, true, true, true},
    // {"binfmt_misc01.sh", true, true, true, true},
    // {"binfmt_misc02.sh", true, true, true, true},
    // {"block_dev", true, true, true, true},
    // {"bpf_map01", true, true, true, true},
    // {"bpf_prog01", true, true, true, true},
    // {"bpf_prog02", true, true, true, true},
    // {"bpf_prog03", true, true, true, true},
    // {"bpf_prog04", true, true, true, true},
    // {"bpf_prog05", true, true, true, true},
    // {"bpf_prog06", true, true, true, true},
    // {"bpf_prog07", true, true, true, true},
    // {"busy_poll_lib.sh", true, true, true, true},
    // {"cacheflush01", true, true, true, true},
    // {"cap_bounds_r", true, true, true, true},
    // {"cap_bounds_rw", true, true, true, true},
    // {"cap_bset_inh_bounds", true, true, true, true},
    // {"capset02", true, true, true, true},
    // {"capset03", true, true, true, true},
    // {"capset04", true, true, true, true},
    // {"cfs_bandwidth01", true, true, true, true},
    // {"cgroup_core01", true, true, true, true},
    // {"cgroup_core02", true, true, true, true},
    // {"cgroup_core03", true, true, true, true},
    // {"cgroup_fj_common.sh", true, true, true, true},
    // {"cgroup_fj_function.sh", true, true, true, true},
    // {"cgroup_fj_proc", true, true, true, true},
    // {"cgroup_fj_stress.sh", true, true, true, true},
    // {"cgroup_lib.sh", true, true, true, true},
    // {"cgroup_regression_3_1.sh", true, true, true, true},
    // {"cgroup_regression_3_2.sh", true, true, true, true},
    // {"cgroup_regression_5_1.sh", true, true, true, true},
    // {"cgroup_regression_5_2.sh", true, true, true, true},
    // {"cgroup_regression_6_1.sh", true, true, true, true},
    // {"cgroup_regression_6_2.sh", true, true, true, true},
    // {"cgroup_regression_fork_processes", true, true, true, true},
    // {"cgroup_regression_getdelays", true, true, true, true},
    // {"cgroup_regression_test.sh", true, true, true, true},
    // {"cgroup_xattr", true, true, true, true},
    // {"change_password.sh", true, true, true, true},
    // {"check_envval", true, true, true, true},
    // {"check_icmpv4_connectivity", true, true, true, true},
    // {"check_icmpv6_connectivity", true, true, true, true},
    // {"check_keepcaps", true, true, true, true},
    // {"check_netem", true, true, true, true},
    // {"check_pe", true, true, true, true},
    // {"check_setkey", true, true, true, true},
    // {"check_simple_capset", true, true, true, true},

    // {"chown01_16", true, true, true, true},
    // {"chown02_16", true, true, true, true},
    // {"chown03_16", true, true, true, true},
    // {"chown04_16", true, true, true, true},
    // {"chown05_16", true, true, true, true},
    // {"chroot01", true, true, true, true},
    // {"chroot02", true, true, true, true},
    // {"chroot04", true, true, true, true},
    // {"cleanup_lvm.sh", true, true, true, true},
    // {"cmdlib.sh", true, true, true, true},
    // {"cn_pec.sh", true, true, true, true},
    // {"cp_tests.sh", true, true, true, true},
    // {"cpio_tests.sh", true, true, true, true},
    // {"cpuacct.sh", true, true, true, true},
    // {"cpuacct_task", true, true, true, true},
    // {"cpuctl_def_task01", true, true, true, true},
    // {"cpuctl_def_task02", true, true, true, true},
    // {"cpuctl_def_task03", true, true, true, true},
    // {"cpuctl_def_task04", true, true, true, true},
    // {"cpuctl_fj_cpu-hog", true, true, true, true},
    // {"cpuctl_fj_simple_echo", true, true, true, true},
    // {"cpuctl_latency_check_task", true, true, true, true},
    // {"cpuctl_latency_test", true, true, true, true},
    // {"cpuctl_test01", true, true, true, true},
    // {"cpuctl_test02", true, true, true, true},
    // {"cpuctl_test03", true, true, true, true},
    // {"cpuctl_test04", true, true, true, true},
    // {"cpufreq_boost", true, true, true, true},
    // {"cpuhotplug_do_disk_write_loop", true, true, true, true},
    // {"cpuhotplug_do_kcompile_loop", true, true, true, true},
    // {"cpuhotplug_do_spin_loop", true, true, true, true},
    // {"cpuhotplug_hotplug.sh", true, true, true, true},
    // {"cpuhotplug_report_proc_interrupts", true, true, true, true},
    // {"cpuhotplug_testsuite.sh", true, true, true, true},
    // {"cpuhotplug01.sh", true, true, true, true},
    // {"cpuhotplug02.sh", true, true, true, true},
    // {"cpuhotplug03.sh", true, true, true, true},
    // {"cpuhotplug04.sh", true, true, true, true},
    // {"cpuhotplug05.sh", true, true, true, true},
    // {"cpuhotplug06.sh", true, true, true, true},
    // {"cpuhotplug07.sh", true, true, true, true},
    // {"cpuset01", true, true, true, true},
    // {"crash01", true, true, true, true},
    // {"crash02", true, true, true, true}, //  acct未实现
    // {"creat07_child", true, true, true, true},
    // {"creat07", true, true, true, true}, //卡死了
    // {"creat09", true, true, true, true}, // mkfs.ext2 failed
    // {"create_datafile", true, true, true, true},
    // {"create_file", true, true, true, true},
    // {"execveat03", true, true, true, true},// TCONF: syscall(281) __NR_execveat not supported on your arch
    // {"crypto_user01", true, true, true, true}, //socket(16, 524290, 21) failed: EAFNOSUPPORT (97)
    // {"crypto_user02", true, true, true, true},
    // {"cve-2014-0196", true, true, true, true},
    // {"cve-2015-3290", true, true, true, true},
    // {"cve-2016-10044", true, true, true, true},
    // {"cve-2016-7042", true, true, true, true},
    // {"cve-2016-7117", true, true, true, true},
    // {"cve-2017-16939", true, true, true, true},
    // {"cve-2017-17052", true, true, true, true},
    // {"cve-2017-17053", true, true, true, true},
    // {"cve-2017-2618", true, true, true, true},
    // {"cve-2017-2671", true, true, true, true},
    // {"cve-2022-4378", true, true, true, true},
    // {"daemonlib.sh", true, true, true, true},
    // {"data", true, true, true, true},
    // {"data_space", true, true, true, true},
    // {"delete_module01", true, true, true, true},
    // {"delete_module02", true, true, true, true},
    // {"delete_module03", true, true, true, true},
    // {"df01.sh", true, true, true, true},
    // {"dhcp_lib.sh", true, true, true, true},
    // {"dio_append", true, true, true, true},
    // {"dio_read", true, true, true, true},
    // {"dio_sparse", true, true, true, true},
    // {"dio_truncate", true, true, true, true},
    // {"diotest1", true, true, true, true},
    // {"diotest2", true, true, true, true},
    // {"diotest3", true, true, true, true},
    // {"diotest5", true, true, true, true},
    // {"diotest6", true, true, true, true},
    // {"dirty", true, true, true, true},
    // {"dirtyc0w", true, true, true, true},
    // {"dirtyc0w_child", true, true, true, true},
    // {"dirtyc0w_shmem", true, true, true, true},
    // {"dirtyc0w_shmem_child", true, true, true, true},
    // {"dirtypipe", true, true, true, true},
    // {"dma_thread_diotest", true, true, true, true},
    // {"dns-stress01-rmt.sh", true, true, true, true},
    // {"dns-stress02-rmt.sh", true, true, true, true},
    // {"dns-stress-lib.sh", true, true, true, true},
    // {"doio", true, true, true, true},
    // {"du01.sh", true, true, true, true},
    // {"dynamic_debug01.sh", true, true, true, true},
    // {"ebizzy", true, true, true, true},
    // {"eject_check_tray", true, true, true, true},
    // {"eject-tests.sh", true, true, true, true},
    // {"endian_switch01", true, true, true, true},
    // {"event_generator", true, true, true, true},
    // {"eventfd04", true, true, true, true},
    // {"eventfd06", true, true, true, true},
    // {"evm_overlay.sh", true, true, true, true},
    // {"exec_with_inh", true, true, true, true},
    // {"exec_without_inh", true, true, true, true},
    // {"execl01_child", true, true, true, true},
    // {"execle01_child", true, true, true, true},
    // {"execlp01_child", true, true, true, true},
    // {"execv01_child", true, true, true, true},
    // {"execve_child", true, true, true, true},
    // {"execve01_child", true, true, true, true},
    // {"execve06_child", true, true, true, true},
    // {"execveat_child", true, true, true, true},
    // {"execveat_errno", true, true, true, true},
    // {"execvp01_child", true, true, true, true},
    // {"copy_file_range02", true, true, true, true},
    // {"copy_file_range03", true, true, true, true},
    // {"f00f", true, true, true, true},
    // {"fanotify_child", true, true, true, true},
    // {"fchown01_16", true, true, true, true},
    // {"fchown02_16", true, true, true, true},
    // {"fchown03_16", true, true, true, true},
    // {"fchown04_16", true, true, true, true},
    // {"fchown05_16", true, true, true, true},
    // {"file01.sh", true, true, true, true},
    // {"filecapstest.sh", true, true, true, true},
    // {"find_portbundle", true, true, true, true},
    // {"finit_module01", true, true, true, true},
    // {"finit_module02", true, true, true, true},
    // {"float_bessel", true, true, true, true},
    // {"float_exp_log", true, true, true, true},
    // {"float_iperb", true, true, true, true},
    // {"float_power", true, true, true, true},
    // {"float_trigo", true, true, true, true},
    // {"force_erase.sh", true, true, true, true},
    // {"fork_exec_loop", true, true, true, true},
    // {"fork_freeze.sh", true, true, true, true},
    // {"fork_procs", true, true, true, true}, // pass1 跑挺久
    // {"fptest01", true, true, true, true},
    // {"fptest02", true, true, true, true},
    // {"frag", true, true, true, true},
    // {"freeze_cancel.sh", true, true, true, true},
    // {"freeze_kill_thaw.sh", true, true, true, true},
    // {"freeze_move_thaw.sh", true, true, true, true},
    // {"freeze_self_thaw.sh", true, true, true, true},
    // {"freeze_sleep_thaw.sh", true, true, true, true},
    // {"freeze_thaw.sh", true, true, true, true},
    // {"freeze_write_freezing.sh", true, true, true, true},
    // {"fs_bind_cloneNS01.sh", true, true, true, true},
    // {"fs_bind_cloneNS02.sh", true, true, true, true},
    // {"fs_bind_cloneNS03.sh", true, true, true, true},
    // {"fs_bind_cloneNS04.sh", true, true, true, true},
    // {"fs_bind_cloneNS05.sh", true, true, true, true},
    // {"fs_bind_cloneNS06.sh", true, true, true, true},
    // {"fs_bind_cloneNS07.sh", true, true, true, true},
    // {"fs_bind_lib.sh", true, true, true, true},
    // {"fs_bind_move01.sh", true, true, true, true},
    // {"fs_bind_move02.sh", true, true, true, true},
    // {"fs_bind_move03.sh", true, true, true, true},
    // {"fs_bind_move04.sh", true, true, true, true},
    // {"fs_bind_move05.sh", true, true, true, true},
    // {"fs_bind_move06.sh", true, true, true, true},
    // {"fs_bind_move07.sh", true, true, true, true},
    // {"fs_bind_move08.sh", true, true, true, true},
    // {"fs_bind_move09.sh", true, true, true, true},
    // {"fs_bind_move10.sh", true, true, true, true},
    // {"fs_bind_move11.sh", true, true, true, true},
    // {"fs_bind_move12.sh", true, true, true, true},
    // {"fs_bind_move13.sh", true, true, true, true},
    // {"fs_bind_move14.sh", true, true, true, true},
    // {"fs_bind_move15.sh", true, true, true, true},
    // {"fs_bind_move16.sh", true, true, true, true},
    // {"fs_bind_move17.sh", true, true, true, true},
    // {"fs_bind_move18.sh", true, true, true, true},
    // {"fs_bind_move19.sh", true, true, true, true},
    // {"fs_bind_move20.sh", true, true, true, true},
    // {"fs_bind_move21.sh", true, true, true, true},
    // {"fs_bind_move22.sh", true, true, true, true},
    // {"fs_bind_rbind01.sh", true, true, true, true},
    // {"fs_bind_rbind02.sh", true, true, true, true},
    // {"fs_bind_rbind03.sh", true, true, true, true},
    // {"fs_bind_rbind04.sh", true, true, true, true},
    // {"fs_bind_rbind05.sh", true, true, true, true},
    // {"fs_bind_rbind06.sh", true, true, true, true},
    // {"fs_bind_rbind07.sh", true, true, true, true},
    // {"fs_bind_rbind07-2.sh", true, true, true, true},
    // {"fs_bind_rbind08.sh", true, true, true, true},
    // {"fs_bind_rbind09.sh", true, true, true, true},
    // {"fs_bind_rbind10.sh", true, true, true, true},
    // {"fs_bind_rbind11.sh", true, true, true, true},
    // {"fs_bind_rbind12.sh", true, true, true, true},
    // {"fs_bind_rbind13.sh", true, true, true, true},
    // {"fs_bind_rbind14.sh", true, true, true, true},
    // {"fs_bind_rbind15.sh", true, true, true, true},
    // {"fs_bind_rbind16.sh", true, true, true, true},
    // {"fs_bind_rbind17.sh", true, true, true, true},
    // {"fs_bind_rbind18.sh", true, true, true, true},
    // {"fs_bind_rbind19.sh", true, true, true, true},
    // {"fs_bind_rbind20.sh", true, true, true, true},
    // {"fs_bind_rbind21.sh", true, true, true, true},
    // {"fs_bind_rbind22.sh", true, true, true, true},
    // {"fs_bind_rbind23.sh", true, true, true, true},
    // {"fs_bind_rbind24.sh", true, true, true, true},
    // {"fs_bind_rbind25.sh", true, true, true, true},
    // {"fs_bind_rbind26.sh", true, true, true, true},
    // {"fs_bind_rbind27.sh", true, true, true, true},
    // {"fs_bind_rbind28.sh", true, true, true, true},
    // {"fs_bind_rbind29.sh", true, true, true, true},
    // {"fs_bind_rbind30.sh", true, true, true, true},
    // {"fs_bind_rbind31.sh", true, true, true, true},
    // {"fs_bind_rbind32.sh", true, true, true, true},
    // {"fs_bind_rbind33.sh", true, true, true, true},
    // {"fs_bind_rbind34.sh", true, true, true, true},
    // {"fs_bind_rbind35.sh", true, true, true, true},
    // {"fs_bind_rbind36.sh", true, true, true, true},
    // {"fs_bind_rbind37.sh", true, true, true, true},
    // {"fs_bind_rbind38.sh", true, true, true, true},
    // {"fs_bind_rbind39.sh", true, true, true, true},
    // {"fs_bind_regression.sh", true, true, true, true},
    // {"fs_bind01.sh", true, true, true, true},
    // {"fs_bind02.sh", true, true, true, true},
    // {"fs_bind03.sh", true, true, true, true},
    // {"fs_bind04.sh", true, true, true, true},
    // {"fs_bind05.sh", true, true, true, true},
    // {"fs_bind06.sh", true, true, true, true},
    // {"fs_bind07.sh", true, true, true, true},
    // {"fs_bind07-2.sh", true, true, true, true},
    // {"fs_bind08.sh", true, true, true, true},
    // {"fs_bind09.sh", true, true, true, true},
    // {"fs_bind10.sh", true, true, true, true},
    // {"fs_bind11.sh", true, true, true, true},
    // {"fs_bind12.sh", true, true, true, true},
    // {"fs_bind13.sh", true, true, true, true},
    // {"fs_bind14.sh", true, true, true, true},
    // {"fs_bind15.sh", true, true, true, true},
    // {"fs_bind16.sh", true, true, true, true},
    // {"fs_bind17.sh", true, true, true, true},
    // {"fs_bind18.sh", true, true, true, true},
    // {"fs_bind19.sh", true, true, true, true},
    // {"fs_bind20.sh", true, true, true, true},
    // {"fs_bind21.sh", true, true, true, true},
    // {"fs_bind22.sh", true, true, true, true},
    // {"fs_bind23.sh", true, true, true, true},
    // {"fs_bind24.sh", true, true, true, true},
    // {"fs_di", true, true, true, true},
    // {"fs_fill", true, true, true, true},
    // {"fs_inod", true, true, true, true},
    // {"fs_perms", true, true, true, true},
    // {"fs_racer.sh", true, true, true, true},
    // {"fs_racer_dir_create.sh", true, true, true, true},
    // {"fs_racer_dir_test.sh", true, true, true, true},
    // {"fs_racer_file_concat.sh", true, true, true, true},
    // {"fs_racer_file_create.sh", true, true, true, true},
    // {"fs_racer_file_link.sh", true, true, true, true},
    // {"fs_racer_file_list.sh", true, true, true, true},
    // {"fs_racer_file_rename.sh", true, true, true, true},
    // {"fs_racer_file_rm.sh", true, true, true, true},
    // {"fs_racer_file_symlink.sh", true, true, true, true},
    // {"fsstress", true, true, true, true},
    // {"fsx-linux", true, true, true, true},
    // {"fsync01", true, true, true, true}, ///dev/block/loop0
    // {"fsync04", true, true, true, true}, ///dev/block/loop0
    // {"ftest01", true, true, true, true},
    // {"ftest02", true, true, true, true},
    // {"ftest03", true, true, true, true},
    // {"ftest04", true, true, true, true},
    // {"ftest05", true, true, true, true},
    // {"ftest06", true, true, true, true},
    // {"ftest07", true, true, true, true},
    // {"ftest08", true, true, true, true},
    // {"ftp-download-stress01-rmt.sh", true, true, true, true},
    // {"ftp-download-stress02-rmt.sh", true, true, true, true},
    // {"ftp-upload-stress01-rmt.sh", true, true, true, true},
    // {"ftp-upload-stress02-rmt.sh", true, true, true, true},
    // {"ftrace_lib.sh", true, true, true, true},
    // {"ftrace_regression01.sh", true, true, true, true},
    // {"ftrace_regression02.sh", true, true, true, true},
    // {"ftrace_stress_test.sh", true, true, true, true},
    // {"ftruncate03_64", true, true, true, true},
    // {"ftruncate04", true, true, true, true},
    // {"ftruncate04_64", true, true, true, true},
    // {"futex_cmp_requeue01", true, true, true, true},
    // {"futex_cmp_requeue02", true, true, true, true},
    // {"futex_wait02", true, true, true, true},
    // {"futex_wait03", true, true, true, true},
    // {"futex_wait05", true, true, true, true},
    // {"futex_waitv01", true, true, true, true},
    // {"futex_waitv02", true, true, true, true},
    // {"futex_waitv03", true, true, true, true},
    // {"futex_wake02", true, true, true, true},
    // {"futex_wake03", true, true, true, true},
    // {"futex_wake04", true, true, true, true},
    // {"futimesat01", true, true, true, true},
    // {"fw_load", true, true, true, true},
    // {"gdb01.sh", true, true, true, true},
    // {"genacos", true, true, true, true},
    // {"genasin", true, true, true, true},
    // {"genatan", true, true, true, true},
    // {"genatan2", true, true, true, true},
    // {"genbessel", true, true, true, true},
    // {"genceil", true, true, true, true},
    // {"gencos", true, true, true, true},
    // {"gencosh", true, true, true, true},
    // {"generate_lvm_runfile.sh", true, true, true, true},
    // {"genexp", true, true, true, true},
    // {"genexp_log", true, true, true, true},
    // {"genfabs", true, true, true, true},
    // {"genfloor", true, true, true, true},
    // {"genfmod", true, true, true, true},
    // {"genfrexp", true, true, true, true},
    // {"genhypot", true, true, true, true},
    // {"geniperb", true, true, true, true},
    // {"genj0", true, true, true, true},
    // {"genj1", true, true, true, true},
    // {"genldexp", true, true, true, true},
    // {"genlgamma", true, true, true, true},
    // {"genload", true, true, true, true},
    // {"genlog", true, true, true, true},
    // {"genlog10", true, true, true, true},
    // {"genmodf", true, true, true, true},
    // {"genpow", true, true, true, true},
    // {"genpower", true, true, true, true},
    // {"gensin", true, true, true, true},
    // {"gensinh", true, true, true, true},
    // {"gensqrt", true, true, true, true},
    // {"gentan", true, true, true, true},
    // {"gentanh", true, true, true, true},
    // {"gentrigo", true, true, true, true},
    // {"geny0", true, true, true, true},
    // {"geny1", true, true, true, true},
    // {"get_ifname", true, true, true, true},
    // {"get_mempolicy01", true, true, true, true},
    // {"get_mempolicy02", true, true, true, true},
    // {"getcontext01", true, true, true, true},
    // {"getcpu01", true, true, true, true}, //sched_setaffinity
    // {"getcwd04", true, true, true, true}, // Test needs at least 2 CPUs online 这个是因为 sched_getaffinity返回0，说不定它不用两个CPU

    // {"getrlimit02", true, true, true, true}, //爆了
    // {"getrlimit03", true, true, true, true}, // 2026-05-21: 四组合定向复测失败，__NR_getrlimit 仍返回 ENOSYS
    // {"getrusage01", true, true, true, true},
    // {"getrusage02", true, true, true, true},
    // {"getrusage03", true, true, true, true},
    // {"getrusage03_child", true, true, true, true},
    // {"getrusage04", true, true, true, true},
    // {"growfiles", true, true, true, true},
    // {"gzip_tests.sh", true, true, true, true},
    // {"hackbench", true, true, true, true},
    // {"hangup01", true, true, true, true},
    // {"ht_affinity", true, true, true, true},
    // {"ht_enabled", true, true, true, true},
    // {"http-stress01-rmt.sh", true, true, true, true},
    // {"http-stress02-rmt.sh", true, true, true, true},
    // {"hugefallocate01", true, true, true, true},
    // {"hugefallocate02", true, true, true, true},
    // {"hugefork01", true, true, true, true},
    // {"hugefork02", true, true, true, true},
    // {"hugemmap01", true, true, true, true},
    // {"hugemmap02", true, true, true, true},
    // {"hugemmap04", true, true, true, true},
    // {"hugemmap05", true, true, true, true},
    // {"hugemmap06", true, true, true, true},
    // {"hugemmap07", true, true, true, true},
    // {"hugemmap08", true, true, true, true},
    // {"hugemmap09", true, true, true, true},
    // {"hugemmap10", true, true, true, true},
    // {"hugemmap11", true, true, true, true},
    // {"hugemmap12", true, true, true, true},
    // {"hugemmap13", true, true, true, true},
    // {"hugemmap14", true, true, true, true},
    // {"hugemmap15", true, true, true, true},
    // {"hugemmap16", true, true, true, true},
    // {"hugemmap17", true, true, true, true},
    // {"hugemmap18", true, true, true, true},
    // {"hugemmap19", true, true, true, true},
    // {"hugemmap20", true, true, true, true},
    // {"hugemmap21", true, true, true, true},
    // {"hugemmap22", true, true, true, true},
    // {"hugemmap23", true, true, true, true},
    // {"hugemmap24", true, true, true, true},
    // {"hugemmap25", true, true, true, true},
    // {"hugemmap26", true, true, true, true},
    // {"hugemmap27", true, true, true, true},
    // {"hugemmap28", true, true, true, true},
    // {"hugemmap29", true, true, true, true},
    // {"hugemmap30", true, true, true, true},
    // {"hugemmap31", true, true, true, true},
    // {"hugemmap32", true, true, true, true},
    // {"hugeshmat01", true, true, true, true},
    // {"hugeshmat02", true, true, true, true},
    // {"hugeshmat03", true, true, true, true},
    // {"hugeshmat04", true, true, true, true},
    // {"hugeshmat05", true, true, true, true},
    // {"hugeshmctl01", true, true, true, true},
    // {"hugeshmctl02", true, true, true, true},
    // {"hugeshmctl03", true, true, true, true},
    // {"hugeshmdt01", true, true, true, true},
    // {"hugeshmget01", true, true, true, true},
    // {"hugeshmget02", true, true, true, true},
    // {"hugeshmget03", true, true, true, true},
    // {"hugeshmget05", true, true, true, true},
    // {"icmp_rate_limit01", true, true, true, true},
    // {"icmp4-multi-diffip01", true, true, true, true},
    // {"icmp4-multi-diffip02", true, true, true, true},
    // {"icmp4-multi-diffip03", true, true, true, true},
    // {"icmp4-multi-diffip04", true, true, true, true},
    // {"icmp4-multi-diffip05", true, true, true, true},
    // {"icmp4-multi-diffip06", true, true, true, true},
    // {"icmp4-multi-diffip07", true, true, true, true},
    // {"icmp4-multi-diffnic01", true, true, true, true},
    // {"icmp4-multi-diffnic02", true, true, true, true},
    // {"icmp4-multi-diffnic03", true, true, true, true},
    // {"icmp4-multi-diffnic04", true, true, true, true},
    // {"icmp4-multi-diffnic05", true, true, true, true},
    // {"icmp4-multi-diffnic06", true, true, true, true},
    // {"icmp4-multi-diffnic07", true, true, true, true},
    // {"icmp6-multi-diffip01", true, true, true, true},
    // {"icmp6-multi-diffip02", true, true, true, true},
    // {"icmp6-multi-diffip03", true, true, true, true},
    // {"icmp6-multi-diffip04", true, true, true, true},
    // {"icmp6-multi-diffip05", true, true, true, true},
    // {"icmp6-multi-diffip06", true, true, true, true},
    // {"icmp6-multi-diffip07", true, true, true, true},
    // {"icmp6-multi-diffnic01", true, true, true, true},
    // {"icmp6-multi-diffnic02", true, true, true, true},
    // {"icmp6-multi-diffnic03", true, true, true, true},
    // {"icmp6-multi-diffnic04", true, true, true, true},
    // {"icmp6-multi-diffnic05", true, true, true, true},
    // {"icmp6-multi-diffnic06", true, true, true, true},
    // {"icmp6-multi-diffnic07", true, true, true, true},
    // {"if-lib.sh", true, true, true, true},
    // {"ima_boot_aggregate", true, true, true, true},
    // {"ima_conditionals.sh", true, true, true, true},
    // {"ima_kexec.sh", true, true, true, true},
    // {"ima_keys.sh", true, true, true, true},
    // {"ima_measurements.sh", true, true, true, true},
    // {"ima_mmap", true, true, true, true},
    // {"ima_policy.sh", true, true, true, true},
    // {"ima_selinux.sh", true, true, true, true},
    // {"ima_setup.sh", true, true, true, true},
    // {"ima_tpm.sh", true, true, true, true},
    // {"ima_violations.sh", true, true, true, true},
    // {"inh_capped", true, true, true, true},
    // {"init_module01", true, true, true, true},
    // {"init_module02", true, true, true, true},
    // {"initialize_if", true, true, true, true},
    // {"inode02", true, true, true, true},
    // {"inotify_init1_01", true, true, true, true},
    // {"inotify_init1_02", true, true, true, true},
    // {"clone303", true, true, true, true}, 
    // {"input01", true, true, true, true},
    // {"input02", true, true, true, true},
    // {"input03", true, true, true, true},
    // {"input04", true, true, true, true},
    // {"input05", true, true, true, true},
    // {"input06", true, true, true, true},
    // {"insmod01.sh", true, true, true, true},
    // {"io_cancel01", true, true, true, true},
    // {"io_cancel02", true, true, true, true},
    // {"io_control01", true, true, true, true},
    // {"io_destroy01", true, true, true, true},
    // {"io_destroy02", true, true, true, true},
    // {"io_getevents01", true, true, true, true},
    // {"io_getevents02", true, true, true, true},
    // {"io_pgetevents01", true, true, true, true},
    // {"io_pgetevents02", true, true, true, true},
    // {"io_setup01", true, true, true, true},
    // {"io_setup02", true, true, true, true},
    // {"io_submit01", true, true, true, true},
    // {"io_submit02", true, true, true, true},
    // {"io_submit03", true, true, true, true},
    // {"io_uring01", true, true, true, true},
    // {"io_uring02", true, true, true, true},
    // {"ioctl_loop01", true, true, true, true},
    // {"ioctl_loop02", true, true, true, true},
    // {"ioctl_loop03", true, true, true, true},
    // {"ioctl_loop04", true, true, true, true},
    // {"ioctl_loop05", true, true, true, true},
    // {"ioctl_loop06", true, true, true, true},
    // {"ioctl_loop07", true, true, true, true},
    // {"ioctl_ns01", true, true, true, true},
    // {"ioctl_ns02", true, true, true, true},
    // {"ioctl_ns03", true, true, true, true},
    // {"ioctl_ns04", true, true, true, true},
    // {"ioctl_ns05", true, true, true, true},
    // {"ioctl_ns06", true, true, true, true},
    // {"ioctl_sg01", true, true, true, true},

    // {"iogen", true, true, true, true},
    // {"ioperm01", true, true, true, true},
    // {"ioperm02", true, true, true, true},
    // {"iopl01", true, true, true, true},
    // {"iopl02", true, true, true, true},
    // {"ioprio_get01", true, true, true, true},
    // {"ioprio_set01", true, true, true, true},
    // {"ioprio_set02", true, true, true, true},
    // {"ioprio_set03", true, true, true, true},
    // {"ipsec_lib.sh", true, true, true, true},
    // {"iptables_lib.sh", true, true, true, true},
    // {"irqbalance01", true, true, true, true},
    // {"isofs.sh", true, true, true, true},
    // {"kallsyms", true, true, true, true},
    // {"kcmp01", true, true, true, true},
    // {"kcmp02", true, true, true, true},
    // {"kcmp03", true, true, true, true},
    // {"kernbench", true, true, true, true},
    // {"keyctl01", true, true, true, true},
    // {"keyctl01.sh", true, true, true, true},
    // {"keyctl02", true, true, true, true},
    // {"keyctl03", true, true, true, true},
    // {"keyctl04", true, true, true, true},
    // {"keyctl05", true, true, true, true},
    // {"keyctl06", true, true, true, true},
    // {"keyctl07", true, true, true, true},
    // {"keyctl08", true, true, true, true},
    // {"keyctl09", true, true, true, true},
    // {"kill02", true, true, true, true},
    // {"kill05", true, true, true, true},
    // {"kill06", true, true, true, true},
    // {"kill07", true, true, true, true},
    // {"kill08", true, true, true, true},
    // {"kill09", true, true, true, true},
    // {"kill10", true, true, true, true},
    // {"kill12", true, true, true, true},
    // {"kill13", true, true, true, true},
    // {"killall_icmp_traffic", true, true, true, true},
    // {"killall_tcp_traffic", true, true, true, true},
    // {"killall_udp_traffic", true, true, true, true},
    // {"kmsg01", true, true, true, true},
    // {"ksm01", true, true, true, true},
    // {"ksm02", true, true, true, true},
    // {"ksm03", true, true, true, true},
    // {"ksm04", true, true, true, true},
    // {"ksm05", true, true, true, true},
    // {"ksm06", true, true, true, true},
    // {"ksm07", true, true, true, true},
    // {"lchown01_16", true, true, true, true},
    // {"lchown02", true, true, true, true},
    // {"lchown02_16", true, true, true, true},
    // {"lchown03", true, true, true, true},
    // {"lchown03_16", true, true, true, true},
    // {"ld01.sh", true, true, true, true},
    // {"ldd01.sh", true, true, true, true},
    // {"leapsec01", true, true, true, true},
    // {"lftest", true, true, true, true},
    // {"lgetxattr01", true, true, true, true},
    // {"lgetxattr02", true, true, true, true},
    // {"libcgroup_freezer", true, true, true, true},
    // {"linktest.sh", true, true, true, true},
    // {"listxattr01", true, true, true, true},
    // {"listxattr02", true, true, true, true},
    // {"listxattr03", true, true, true, true},
    // {"llistxattr01", true, true, true, true},
    // {"llistxattr02", true, true, true, true},
    // {"llistxattr03", true, true, true, true},
    // {"ln_tests.sh", true, true, true, true},
    // {"lock_torture.sh", true, true, true, true},
    // {"locktests", true, true, true, true},
    // {"logrotate_tests.sh", true, true, true, true},
    // {"lremovexattr01", true, true, true, true},
    // {"lseek11", true, true, true, true}, //SEEK_DATA and SEEK_HOLE not implemented
    // {"lsmod01.sh", true, true, true, true},
    // {"ltp_acpi", true, true, true, true},
    // {"ltpClient", true, true, true, true},
    // {"ltpServer", true, true, true, true},
    // {"ltpSockets.sh", true, true, true, true},
    // {"macsec_lib.sh", true, true, true, true},
    // {"madvise02", true, true, true, true},
    // {"madvise03", true, true, true, true},
    // {"madvise06", true, true, true, true},
    // {"madvise07", true, true, true, true},
    // {"madvise08", true, true, true, true},
    // {"madvise09", true, true, true, true},
    // {"madvise11", true, true, true, true},
    // {"mallinfo01", true, true, true, true},
    // {"mallinfo02", true, true, true, true},
    // {"mallinfo2_01", true, true, true, true},
    // {"mallocstress", true, true, true, true},
    // {"mallopt01", true, true, true, true},
    // {"max_map_count", true, true, true, true}, ///proc/sys/vm/overcommit_memory
    // {"mbind01", true, true, true, true},
    // {"mbind02", true, true, true, true},
    // {"mbind03", true, true, true, true},
    // {"mbind04", true, true, true, true},
    // {"mc_member_test", true, true, true, true},
    // {"mc_recv", true, true, true, true},
    // {"mc_send", true, true, true, true},
    // {"mc_verify_opts", true, true, true, true},
    // {"mc_verify_opts_error", true, true, true, true},
    // {"mcast-lib.sh", true, true, true, true},
    // {"meltdown", true, true, true, true},
    // {"mem_process", true, true, true, true},
    // {"mem02", true, true, true, true}, //过了但是没有summary
    // {"membarrier01", true, true, true, true},
    // {"memcg_control_test.sh", true, true, true, true},
    // {"memcg_failcnt.sh", true, true, true, true},
    // {"memcg_force_empty.sh", true, true, true, true},
    // {"memcg_lib.sh", true, true, true, true},
    // {"memcg_limit_in_bytes.sh", true, true, true, true},
    // {"memcg_max_usage_in_bytes_test.sh", true, true, true, true},
    // {"memcg_memsw_limit_in_bytes_test.sh", true, true, true, true},
    // {"memcg_move_charge_at_immigrate_test.sh", true, true, true, true},
    // {"memcg_process", true, true, true, true},
    // {"memcg_process_stress", true, true, true, true},
    // {"memcg_regression_test.sh", true, true, true, true},
    // {"memcg_stat_rss.sh", true, true, true, true},
    // {"memcg_stat_test.sh", true, true, true, true},
    // {"memcg_stress_test.sh", true, true, true, true},
    // {"memcg_subgroup_charge.sh", true, true, true, true},
    // {"memcg_test_1", true, true, true, true},
    // {"memcg_test_2", true, true, true, true},
    // {"memcg_test_3", true, true, true, true},
    // {"memcg_test_4", true, true, true, true},
    // {"memcg_test_4.sh", true, true, true, true},
    // {"memcg_usage_in_bytes_test.sh", true, true, true, true},
    // {"memcg_use_hierarchy_test.sh", true, true, true, true},
    // {"memcontrol01", true, true, true, true}, ///proc/self/mounts
    // {"memcontrol02", true, true, true, true}, ///dev/block/loop0
    // {"memcontrol03", true, true, true, true}, ///dev/block/loop0
    // {"memcontrol04", true, true, true, true}, ///dev/block/loop0连着跑似乎就变成loop1和loop2了
    // {"memctl_test01", true, true, true, true},
    // {"memfd_create03", true, true, true, true},
    // {"memfd_create04", true, true, true, true},
    // {"memset01", true, true, true, true}, // passed   1
    // {"memtoy", true, true, true, true},
    // {"mesgq_nstest", true, true, true, true},
    // {"migrate_pages01", true, true, true, true},
    // {"migrate_pages02", true, true, true, true},
    // {"migrate_pages03", true, true, true, true},
    // {"min_free_kbytes", true, true, true, true}, ///proc/sys/vm/overcommit_memory
    // {"mincore01", true, true, true, true},
    // {"mincore02", true, true, true, true},
    // {"mincore03", true, true, true, true},
    // {"mincore04", true, true, true, true},
    // {"mkdir_tests.sh", true, true, true, true},
    // {"mkdirat01", true, true, true, true}, // pass
    // {"mkfs01.sh", true, true, true, true},
    // {"mknod03", true, true, true, true},
    // {"mknod04", true, true, true, true},
    // {"mknod05", true, true, true, true},
    // {"mknod06", true, true, true, true},
    // {"mknod08", true, true, true, true},
    // {"mknodat01", true, true, true, true},
    // {"mknodat02", true, true, true, true},
    // {"mkswap01.sh", true, true, true, true},
    // {"mlock02", true, true, true, true},
    // {"mlock03", true, true, true, true},
    // {"mlock04", true, true, true, true},
    // {"mlock05", true, true, true, true},
    // {"mlock201", true, true, true, true},
    // {"mlock202", true, true, true, true},
    // {"mlock203", true, true, true, true},
    // {"mlockall01", true, true, true, true},
    // {"mlockall02", true, true, true, true},
    // {"mlockall03", true, true, true, true},
    // {"mmstress", true, true, true, true},
    // {"mmstress_dummy", true, true, true, true},
    // {"modify_ldt01", true, true, true, true},
    // {"modify_ldt02", true, true, true, true},
    // {"modify_ldt03", true, true, true, true},
    // {"mount_setattr01", true, true, true, true},
    // {"mount01", true, true, true, true}, ///dev/loop0
    // {"mount02", true, true, true, true},
    // {"mount03", true, true, true, true},
    // {"mount03_suid_child", true, true, true, true},
    // {"mount04", true, true, true, true},
    // {"mount05", true, true, true, true},
    // {"mount06", true, true, true, true},
    // {"mount07", true, true, true, true},
    // {"mountns01", true, true, true, true},
    // {"mountns02", true, true, true, true},
    // {"mountns03", true, true, true, true},
    // {"mountns04", true, true, true, true},
    // {"move_mount01", true, true, true, true},
    // {"move_mount02", true, true, true, true},
    // {"move_pages01", true, true, true, true},
    // {"move_pages02", true, true, true, true},
    // {"move_pages03", true, true, true, true},
    // {"move_pages04", true, true, true, true},
    // {"move_pages05", true, true, true, true},
    // {"move_pages06", true, true, true, true},
    // {"move_pages07", true, true, true, true},
    // {"move_pages09", true, true, true, true},
    // {"move_pages10", true, true, true, true},
    // {"move_pages11", true, true, true, true},
    // {"move_pages12", true, true, true, true},
    // {"mpls_lib.sh", true, true, true, true},
    // {"mprotect01", true, true, true, true}, //无所谓，没summary
    // {"mprotect02", true, true, true, true}, //无所谓，没summary
    // {"mprotect03", true, true, true, true}, //无所谓，没summary
    // {"mprotect04", true, true, true, true}, //无所谓，没summary
    // {"mprotect05", true, true, true, true}, //pass 1 fail1
    // {"mq_notify01", true, true, true, true},
    // {"mq_notify02", true, true, true, true},
    // {"mq_notify03", true, true, true, true},
    // {"mq_open01", true, true, true, true},
    // {"mq_timedreceive01", true, true, true, true}, // 2026-05-21: 四组合定向复测失败，mq_open 仍返回 ENOSYS
    // {"mq_timedsend01", true, true, true, true}, // 2026-05-21: 四组合定向复测失败，mq_open 仍返回 ENOSYS
    // {"mq_unlink01", true, true, true, true},
    // {"mqns_01", true, true, true, true},
    // {"mqns_02", true, true, true, true},
    // {"mqns_03", true, true, true, true},
    // {"mqns_04", true, true, true, true},
    // {"mremap01", true, true, true, true}, // pass 没summary
    // {"mremap02", true, true, true, true}, // pass 没summary
    // {"mremap03", true, true, true, true}, // pass 没summary
    // {"mremap04", true, true, true, true}, // pass 没summary
    // {"mremap05", true, true, true, true}, // pass 没summary
    // {"mremap06", true, true, true, true},
    // {"msg_comm", true, true, true, true},
    // {"msgctl01", true, true, true, true},
    // {"msgctl02", true, true, true, true},
    // {"msgctl03", true, true, true, true},
    // {"msgctl04", true, true, true, true},
    // {"msgctl05", true, true, true, true},
    // {"msgctl06", true, true, true, true},
    // {"msgctl12", true, true, true, true},
    // {"msgget01", true, true, true, true},
    // {"msgget02", true, true, true, true},
    // {"msgget03", true, true, true, true},
    // {"msgget04", true, true, true, true},
    // {"msgget05", true, true, true, true},
    // {"msgrcv01", true, true, true, true},
    // {"msgrcv02", true, true, true, true},
    // {"msgrcv03", true, true, true, true},
    // {"msgrcv05", true, true, true, true},
    // {"msgrcv06", true, true, true, true},
    // {"msgrcv07", true, true, true, true},
    // {"msgrcv08", true, true, true, true},
    // {"msgsnd01", true, true, true, true},
    // {"msgsnd02", true, true, true, true},
    // {"msgsnd05", true, true, true, true},
    // {"msgsnd06", true, true, true, true},
    // {"msgstress01", true, true, true, true},
    // {"msync03", true, true, true, true}, //pass
    // {"msync04", true, true, true, true}, ///dev/loop0
    // {"mtest01", true, true, true, true},
    // {"munlock02", true, true, true, true},
    // {"munlockall01", true, true, true, true},
    // {"munmap01", true, true, true, true}, // pass 没summary
    // {"munmap02", true, true, true, true}, // pass 没summary
    // {"munmap03", true, true, true, true}, // pass 没summary
    // {"mv_tests.sh", true, true, true, true},
    // {"myfunctions.sh", true, true, true, true},
    // {"name_to_handle_at01", true, true, true, true},
    // {"name_to_handle_at02", true, true, true, true},
    // {"nanosleep01", true, true, true, true},
    // {"net_cmdlib.sh", true, true, true, true},
    // {"netns_breakns.sh", true, true, true, true},
    // {"netns_comm.sh", true, true, true, true},
    // {"netns_lib.sh", true, true, true, true},
    // {"netns_netlink", true, true, true, true},
    // {"netns_sysfs.sh", true, true, true, true},
    // {"netstress", true, true, true, true},
    // {"nfs_flock", true, true, true, true},
    // {"nfs_flock_dgen", true, true, true, true},
    // {"nfs_lib.sh", true, true, true, true},
    // {"nfs01_open_files", true, true, true, true},
    // {"nfs04_create_file", true, true, true, true},
    // {"nfs05_make_tree", true, true, true, true},
    // {"nft02", true, true, true, true},
    // {"nftw01", true, true, true, true},
    // {"nftw6401", true, true, true, true},
    // {"nice03", true, true, true, true},
    // {"nice05", true, true, true, true},
    // {"nm01.sh", true, true, true, true},
    // {"nptl01", true, true, true, true},
    // {"ns-echoclient", true, true, true, true},
    // {"ns-icmp_redirector", true, true, true, true},
    // {"ns-icmpv4_sender", true, true, true, true},
    // {"ns-icmpv6_sender", true, true, true, true},
    // {"ns-igmp_querier", true, true, true, true},
    // {"ns-mcast_join", true, true, true, true},
    // {"ns-mcast_receiver", true, true, true, true},
    // {"ns-tcpclient", true, true, true, true},
    // {"ns-tcpserver", true, true, true, true},
    // {"ns-udpclient", true, true, true, true},
    // {"ns-udpsender", true, true, true, true},
    // {"ns-udpserver", true, true, true, true},
    // {"numa01.sh", true, true, true, true},
    // {"oom01", true, true, true, true},
    // {"oom02", true, true, true, true},
    // {"oom03", true, true, true, true},
    // {"oom04", true, true, true, true},
    // {"oom05", true, true, true, true},
    // {"open_by_handle_at01", true, true, true, true},
    // {"open_by_handle_at02", true, true, true, true},
    // {"open_tree01", true, true, true, true},
    // {"open12", true, true, true, true}, //过三个
    // {"open12_child", true, true, true, true}, //这个不是测例
    // {"open13", true, true, true, true}, // pass
    // {"open14", true, true, true, true}, //pass这个测例要跑一年，别急着掐死，多等会
    // {"openat02_child", true, true, true, true},
    // {"openat03", true, true, true, true}, //pass这个和那个一年是同一个
    // {"openat04", true, true, true, true}, ///dev/block/loop0
    // {"openat201", true, true, true, true},
    // {"openat202", true, true, true, true},
    // {"openat203", true, true, true, true},
    // {"openfile", true, true, true, true},
    // {"output_ipsec_conf", true, true, true, true},
    // {"overcommit_memory", true, true, true, true},
    // {"page01", true, true, true, true},
    // {"page02", true, true, true, true},
    // {"parameters.sh", true, true, true, true},
    // {"pause01", true, true, true, true},
    // {"pause02", true, true, true, true},
    // {"pause03", true, true, true, true},
    // {"pcrypt_aead01", true, true, true, true},
    // {"pec_listener", true, true, true, true},
    // {"perf_event_open01", true, true, true, true},
    // {"perf_event_open02", true, true, true, true},
    // {"perf_event_open03", true, true, true, true},
    // {"pidfd_getfd01", true, true, true, true},
    // {"pidfd_getfd02", true, true, true, true},
    // {"pidfd_open01", true, true, true, true},
    // {"pidfd_open02", true, true, true, true},
    // {"pidfd_open03", true, true, true, true},
    // {"pidfd_open04", true, true, true, true},
    // {"pidfd_send_signal01", true, true, true, true},
    // {"pidfd_send_signal02", true, true, true, true},
    // {"pidfd_send_signal03", true, true, true, true},
    // {"pidns01", true, true, true, true},
    // {"pidns02", true, true, true, true},
    // {"pidns03", true, true, true, true},
    // {"pidns04", true, true, true, true},
    // {"pidns05", true, true, true, true},
    // {"pidns06", true, true, true, true},
    // {"pidns10", true, true, true, true},
    // {"pidns12", true, true, true, true},
    // {"pidns13", true, true, true, true},
    // {"pidns16", true, true, true, true},
    // {"pidns17", true, true, true, true},
    // {"pidns20", true, true, true, true},
    // {"pidns30", true, true, true, true},
    // {"pidns31", true, true, true, true},
    // {"pidns32", true, true, true, true},
    // {"pids.sh", true, true, true, true},
    // {"pids_task1", true, true, true, true},
    // {"pids_task2", true, true, true, true},
    // {"pipe02", true, true, true, true},
    // {"pipe05", true, true, true, true}, // 完全PASS
    // {"pipe07", true, true, true, true}, //proc/self/fd没写
    // {"pipe09", true, true, true, true}, // 完全PASS
    // {"pipe13", true, true, true, true}, // proc/4/stat没写
    // {"pipe15", true, true, true, true}, //NOFILE limit max too low: 128 < 65536
    // {"pipe2_02", true, true, true, true},
    // {"pipe2_02_child", true, true, true, true},
    // {"pipe2_04", true, true, true, true},
    // {"pipeio", true, true, true, true},
    // {"pivot_root01", true, true, true, true},
    // {"pkey01", true, true, true, true},
    // {"pm_cpu_consolidation.py", true, true, true, true},
    // {"pm_get_sched_values", true, true, true, true},
    // {"pm_ilb_test.py", true, true, true, true},
    // {"pm_include.sh", true, true, true, true},
    // {"pm_sched_domain.py", true, true, true, true},
    // {"pm_sched_mc.py", true, true, true, true},
    // {"poll02", true, true, true, true},
    // {"posix_fadvise04", true, true, true, true},
    // {"posix_fadvise04_64", true, true, true, true},
    // {"ppoll01", true, true, true, true}, // 2026-05-21: 四组合定向复测异常，RV 在 MASK_SIGNAL 子场景卡住，LA 在同子场景 kerneltrap
    // {"prctl03", true, true, true, true},
    // {"prctl04", true, true, true, true},
    // {"prctl05", true, true, true, true},
    // {"prctl06", true, true, true, true},
    // {"prctl06_execve", true, true, true, true},
    // {"prctl07", true, true, true, true},
    // {"prctl08", true, true, true, true},
    // {"prctl09", true, true, true, true},
    // {"prctl10", true, true, true, true},
    // {"preadv01", true, true, true, true},
    // {"preadv01_64", true, true, true, true},
    // {"preadv02", true, true, true, true},
    // {"preadv02_64", true, true, true, true},
    // {"preadv03", true, true, true, true},
    // {"preadv03_64", true, true, true, true},
    // {"preadv201", true, true, true, true},
    // {"preadv201_64", true, true, true, true},
    // {"preadv202", true, true, true, true},
    // {"preadv202_64", true, true, true, true},
    // {"preadv203", true, true, true, true},
    // {"preadv203_64", true, true, true, true},
    // {"prepare_lvm.sh", true, true, true, true},
    // {"print_caps", true, true, true, true},
    // {"proc_sched_rt01", true, true, true, true},
    // {"proc01", true, true, true, true}, //pass
    // {"process_madvise01", true, true, true, true},
    // {"process_vm_readv02", true, true, true, true},
    // {"process_vm_readv03", true, true, true, true},
    // {"process_vm_writev02", true, true, true, true},
    // {"process_vm01", true, true, true, true},
    // {"profil01", true, true, true, true},
    // {"prot_hsymlinks", true, true, true, true},
    // {"pselect01", true, true, true, true}, // /bin/sh
    // {"pselect01_64", true, true, true, true},
    // {"pt_test", true, true, true, true},
    // {"ptem01", true, true, true, true},
    // {"pth_str01", true, true, true, true},
    // {"pth_str02", true, true, true, true},
    // {"pth_str03", true, true, true, true},
    // {"pthcli", true, true, true, true},
    // {"pthserv", true, true, true, true},
    // {"ptrace01", true, true, true, true},
    // {"ptrace02", true, true, true, true},
    // {"ptrace03", true, true, true, true},
    // {"ptrace04", true, true, true, true},
    // {"ptrace05", true, true, true, true},
    // {"ptrace06", true, true, true, true},
    // {"ptrace07", true, true, true, true},
    // {"ptrace08", true, true, true, true},
    // {"ptrace09", true, true, true, true},
    // {"ptrace10", true, true, true, true},
    // {"ptrace11", true, true, true, true},
    // {"pty01", true, true, true, true},
    // {"pty02", true, true, true, true},
    // {"pty03", true, true, true, true},
    // {"pty04", true, true, true, true},
    // {"pty05", true, true, true, true},
    // {"pty06", true, true, true, true},
    // {"pty07", true, true, true, true},
    // {"pwrite03", true, true, true, true},
    // {"pwrite03_64", true, true, true, true},
    // {"pwrite04", true, true, true, true},
    // {"pwrite04_64", true, true, true, true},
    // {"pwritev01", true, true, true, true},
    // {"pwritev01_64", true, true, true, true},
    // {"pwritev02", true, true, true, true},
    // {"pwritev02_64", true, true, true, true},
    // {"pwritev03", true, true, true, true},
    // {"pwritev03_64", true, true, true, true},
    // {"pwritev201", true, true, true, true},
    // {"pwritev201_64", true, true, true, true},
    // {"pwritev202", true, true, true, true},
    // {"pwritev202_64", true, true, true, true},
    // {"quota_remount_test01.sh", true, true, true, true},
    // {"quotactl01", true, true, true, true},
    // {"quotactl02", true, true, true, true},
    // {"quotactl03", true, true, true, true},
    // {"quotactl04", true, true, true, true},
    // {"quotactl05", true, true, true, true},
    // {"quotactl06", true, true, true, true},
    // {"quotactl07", true, true, true, true},
    // {"quotactl08", true, true, true, true},
    // {"quotactl09", true, true, true, true},
    // {"rcu_torture.sh", true, true, true, true},
    // {"read_all", true, true, true, true},
    // {"readahead01", true, true, true, true},
    // {"readahead02", true, true, true, true},
    // {"readdir21", true, true, true, true},
    // {"realpath01", true, true, true, true},
    // {"reboot01", true, true, true, true},
    // {"reboot02", true, true, true, true},
    // {"remap_file_pages01", true, true, true, true},
    // {"remap_file_pages02", true, true, true, true},
    // {"remove_password.sh", true, true, true, true},
    // {"removexattr01", true, true, true, true},
    // {"removexattr02", true, true, true, true},
    // {"rename09", true, true, true, true},
    // {"rename11", true, true, true, true},
    // {"rename12", true, true, true, true},
    // {"rename14", true, true, true, true},
    // {"renameat201", true, true, true, true},
    // {"renameat202", true, true, true, true},
    // {"request_key01", true, true, true, true},
    // {"request_key02", true, true, true, true},
    // {"request_key03", true, true, true, true},
    // {"request_key04", true, true, true, true},
    // {"request_key05", true, true, true, true},
    // {"route4-rmmod", true, true, true, true},
    // {"route6-rmmod", true, true, true, true},
    // {"route-change-netlink", true, true, true, true},
    // {"route-lib.sh", true, true, true, true},
    // {"rt_sigprocmask01", true, true, true, true},
    // {"rt_sigprocmask02", true, true, true, true},
    // {"rt_sigqueueinfo01", true, true, true, true},
    // {"rt_sigsuspend01", true, true, true, true},
    // {"rtc01", true, true, true, true},
    // {"rtc02", true, true, true, true},
    // {"run_capbounds.sh", true, true, true, true},
    // {"run_cpuctl_latency_test.sh", true, true, true, true},
    // {"run_cpuctl_stress_test.sh", true, true, true, true},
    // {"run_cpuctl_test.sh", true, true, true, true},
    // {"run_cpuctl_test_fj.sh", true, true, true, true},
    // {"run_freezer.sh", true, true, true, true},
    // {"run_memctl_test.sh", true, true, true, true},
    // {"run_sched_cliserv.sh", true, true, true, true},
    // {"runpwtests_exclusive01.sh", true, true, true, true},
    // {"runpwtests_exclusive02.sh", true, true, true, true},
    // {"runpwtests_exclusive03.sh", true, true, true, true},
    // {"runpwtests_exclusive04.sh", true, true, true, true},
    // {"runpwtests_exclusive05.sh", true, true, true, true},
    // {"runpwtests01.sh", true, true, true, true},
    // {"runpwtests02.sh", true, true, true, true},
    // {"runpwtests03.sh", true, true, true, true},
    // {"runpwtests04.sh", true, true, true, true},
    // {"runpwtests05.sh", true, true, true, true},
    // {"runpwtests06.sh", true, true, true, true},
    // {"rwtest", true, true, true, true},
    // {"sbrk01", true, true, true, true}, // 爆了
    // {"sbrk02", true, true, true, true}, // pass
    // {"sbrk03", true, true, true, true}, // Arch需要是S390
    // {"sched_datafile", true, true, true, true},
    // {"sched_driver", true, true, true, true},
    // {"sched_get_priority_max01", true, true, true, true},
    // {"sched_get_priority_max02", true, true, true, true},
    // {"sched_get_priority_min01", true, true, true, true},
    // {"sched_get_priority_min02", true, true, true, true},
    // {"sched_getattr01", true, true, true, true},
    // {"sched_getattr02", true, true, true, true},
    // {"sched_getparam01", true, true, true, true},
    // {"sched_getparam03", true, true, true, true},
    // {"sched_getscheduler01", true, true, true, true},
    // {"sched_getscheduler02", true, true, true, true},
    // {"sched_rr_get_interval01", true, true, true, true},
    // {"sched_rr_get_interval02", true, true, true, true},
    // {"sched_rr_get_interval03", true, true, true, true},
    // {"sched_setaffinity01", true, true, true, true},
    // {"sched_setattr01", true, true, true, true},
    // {"sched_setparam01", true, true, true, true},
    // {"sched_setparam02", true, true, true, true},
    // {"sched_setparam03", true, true, true, true},
    // {"sched_setparam04", true, true, true, true},
    // {"sched_setparam05", true, true, true, true},
    // {"sched_setscheduler01", true, true, true, true},
    // {"sched_setscheduler02", true, true, true, true},
    // {"sched_setscheduler03", true, true, true, true},
    // {"sched_stress.sh", true, true, true, true},
    // {"sched_tc0", true, true, true, true},
    // {"sched_tc1", true, true, true, true},
    // {"sched_tc2", true, true, true, true},
    // {"sched_tc3", true, true, true, true},
    // {"sched_tc4", true, true, true, true},
    // {"sched_tc5", true, true, true, true},
    // {"sched_tc6", true, true, true, true},
    // {"sched_yield01", true, true, true, true}, // pass
    // {"select02", true, true, true, true},
    // {"select04", true, true, true, true},
    // {"sem_comm", true, true, true, true},
    // {"sem_nstest", true, true, true, true},
    // {"semctl01", true, true, true, true},
    // {"semctl02", true, true, true, true},
    // {"semctl03", true, true, true, true},
    // {"semctl04", true, true, true, true},
    // {"semctl05", true, true, true, true},
    // {"semctl06", true, true, true, true},
    // {"semctl07", true, true, true, true},
    // {"semctl08", true, true, true, true},
    // {"semctl09", true, true, true, true},
    // {"semget01", true, true, true, true},
    // {"semget02", true, true, true, true},
    // {"semget05", true, true, true, true},
    // {"semop01", true, true, true, true},
    // {"semop02", true, true, true, true},
    // {"semop03", true, true, true, true},
    // {"semop04", true, true, true, true},
    // {"semop05", true, true, true, true},
    // {"semtest_2ns", true, true, true, true},
    // {"sendfile02", true, true, true, true},
    // {"sendfile02_64", true, true, true, true},
    // {"sendfile03", true, true, true, true},
    // {"sendfile03_64", true, true, true, true},
    // {"sendfile04", true, true, true, true},
    // {"sendfile04_64", true, true, true, true},
    // {"sendfile05", true, true, true, true},
    // {"sendfile05_64", true, true, true, true},
    // {"sendfile06", true, true, true, true},
    // {"sendfile06_64", true, true, true, true},
    // {"sendfile07", true, true, true, true},
    // {"sendfile07_64", true, true, true, true},
    // {"sendfile08", true, true, true, true},
    // {"sendfile08_64", true, true, true, true},
    // {"sendfile09", true, true, true, true},
    // {"sendfile09_64", true, true, true, true},
    // {"set_ipv4addr", true, true, true, true},
    // {"setresuid01_16", true, true, true, true},
    // {"setresuid02_16", true, true, true, true},
    // {"setresuid03_16", true, true, true, true},
    // {"setresuid04_16", true, true, true, true},
    // {"setresuid05_16", true, true, true, true},
    // {"set_mempolicy01", true, true, true, true},
    // {"set_mempolicy02", true, true, true, true},
    // {"set_mempolicy03", true, true, true, true},
    // {"set_mempolicy04", true, true, true, true},
    // {"set_mempolicy05", true, true, true, true},
    // {"set_thread_area01", true, true, true, true},
    // {"setdomainname01", true, true, true, true},
    // {"setdomainname02", true, true, true, true},
    // {"setdomainname03", true, true, true, true},
    // {"setfsgid01_16", true, true, true, true},
    // {"setfsgid02_16", true, true, true, true},
    // {"setfsgid03", true, true, true, true},
    // {"setfsgid03_16", true, true, true, true},
    // {"setfsuid01_16", true, true, true, true},
    // {"setfsuid02", true, true, true, true},
    // {"setfsuid02_16", true, true, true, true},
    // {"setfsuid03_16", true, true, true, true},
    // {"setfsuid04", true, true, true, true},
    // {"setfsuid04_16", true, true, true, true},
    // {"setgid01_16", true, true, true, true},
    // {"setgid02_16", true, true, true, true},
    // {"setgid03_16", true, true, true, true},
    // {"setgroups01_16", true, true, true, true},
    // {"setgroups02_16", true, true, true, true},
    // {"setgroups03", true, true, true, true},
    // {"setgroups03_16", true, true, true, true},
    // {"setgroups04", true, true, true, true},
    // {"setgroups04_16", true, true, true, true},
    // {"sethostname01", true, true, true, true},
    // {"sethostname02", true, true, true, true},
    // {"sethostname03", true, true, true, true},
    // {"setitimer01", true, true, true, true},
    // {"setns01", true, true, true, true},
    // {"setns02", true, true, true, true},
    // {"setpgid02", true, true, true, true}, // pass
    // {"setpgid03", true, true, true, true}, // 要完善sid逻辑, 而且现在退不出去, 先不修
    // {"setpgid03_child", true, true, true, true},
    // {"setpriority01", true, true, true, true}, // 原始 LTP 用例依赖 useradd/userdel；当前按约束不补用户管理命令，因此不接入默认回归
    // {"setregid01_16", true, true, true, true},
    // {"setregid02_16", true, true, true, true},
    // {"setregid03_16", true, true, true, true},
    // {"setregid04_16", true, true, true, true},
    // {"setresgid01_16", true, true, true, true},
    // {"setresgid02_16", true, true, true, true},
    // {"setresgid03_16", true, true, true, true},
    // {"setresgid04_16", true, true, true, true},
    // {"setreuid01_16", true, true, true, true},
    // {"setreuid02_16", true, true, true, true},
    // {"setreuid03_16", true, true, true, true},
    // {"setreuid04_16", true, true, true, true},
    // {"setreuid05_16", true, true, true, true},
    // {"setreuid06_16", true, true, true, true},
    // {"setreuid07_16", true, true, true, true},
    // {"setrlimit01", true, true, true, true},
    // {"setrlimit03", true, true, true, true},
    // {"setrlimit05", true, true, true, true},
    // {"setrlimit06", true, true, true, true},
    // {"setuid01_16", true, true, true, true},
    // {"setuid03_16", true, true, true, true},
    // {"setuid04", true, true, true, true},
    // {"setuid04_16", true, true, true, true},
    // {"setxattr02", true, true, true, true},
    // {"setxattr03", true, true, true, true},
    // {"sgetmask01", true, true, true, true},
    // {"shell_pipe01.sh", true, true, true, true},
    // {"sighold02", true, true, true, true},
    // {"signal01", true, true, true, true},
    // {"signal06", true, true, true, true},
    // {"signalfd4_01", true, true, true, true},
    // {"signalfd4_02", true, true, true, true},
    // {"sigrelse01", true, true, true, true},
    // {"sigsuspend01", true, true, true, true},
    // {"sigtimedwait01", true, true, true, true},
    // {"sigwait01", true, true, true, true},
    // {"sigwaitinfo01", true, true, true, true},
    // {"smack_common.sh", true, true, true, true},
    // {"smack_file_access.sh", true, true, true, true},
    // {"smack_notroot", true, true, true, true},
    // {"smack_set_ambient.sh", true, true, true, true},
    // {"smack_set_cipso.sh", true, true, true, true},
    // {"smack_set_current.sh", true, true, true, true},
    // {"smack_set_direct.sh", true, true, true, true},
    // {"smack_set_doi.sh", true, true, true, true},
    // {"smack_set_load.sh", true, true, true, true},
    // {"smack_set_netlabel.sh", true, true, true, true},
    // {"smack_set_onlycap.sh", true, true, true, true},
    // {"smack_set_socket_labels", true, true, true, true},
    // {"smt_smp_affinity.sh", true, true, true, true},
    // {"smt_smp_enabled.sh", true, true, true, true},
    // {"snd_seq01", true, true, true, true},
    // {"snd_timer01", true, true, true, true},
    // {"squashfs01", true, true, true, true},
    // {"ssetmask01", true, true, true, true},
    // {"stack_clash", true, true, true, true},
    // {"stack_space", true, true, true, true},
    // {"starvation", true, true, true, true},
    // {"statfs03", true, true, true, true}, //爆了
    // {"statfs03_64", true, true, true, true},
    // {"statx04", true, true, true, true}, //bin/sh
    // {"statx05", true, true, true, true},
    // {"statx06", true, true, true, true},
    // {"statx07", true, true, true, true},
    // {"statx08", true, true, true, true},
    // {"statx09", true, true, true, true}, //.config
    // {"statx10", true, true, true, true}, //bin/sh
    // {"statx11", true, true, true, true},
    // {"statx12", true, true, true, true},
    // {"stime01", true, true, true, true},
    // {"stime02", true, true, true, true},
    // {"stop_freeze_sleep_thaw_cont.sh", true, true, true, true},
    // {"stop_freeze_thaw_cont.sh", true, true, true, true},
    // {"stress", true, true, true, true},
    // {"support_numa", true, true, true, true},
    // {"swapoff01", true, true, true, true},
    // {"swapoff02", true, true, true, true},
    // {"swapon01", true, true, true, true},
    // {"swapon02", true, true, true, true},
    // {"swapon03", true, true, true, true},
    // {"swapping01", true, true, true, true},
    // {"sync_file_range01", true, true, true, true},
    // {"sync_file_range02", true, true, true, true},
    // {"sync01", true, true, true, true},
    // {"syncfs01", true, true, true, true},
    // {"sysctl01", true, true, true, true},
    // {"sysctl01.sh", true, true, true, true},
    // {"sysctl02.sh", true, true, true, true},
    // {"sysctl03", true, true, true, true},
    // {"sysctl04", true, true, true, true},
    // {"sysfs01", true, true, true, true},
    // {"sysfs02", true, true, true, true},
    // {"sysfs03", true, true, true, true},
    // {"sysfs04", true, true, true, true},
    // {"sysfs05", true, true, true, true},
    // {"sysinfo03", true, true, true, true},
    // {"syslog11", true, true, true, true},
    // {"syslog12", true, true, true, true},
    // {"tar_tests.sh", true, true, true, true},
    // {"tbio", true, true, true, true},
    // {"tcindex01", true, true, true, true},
    // {"tcp_cc_lib.sh", true, true, true, true},
    // {"tcp4-multi-diffip01", true, true, true, true},
    // {"tcp4-multi-diffip02", true, true, true, true},
    // {"tcp4-multi-diffip03", true, true, true, true},
    // {"tcp4-multi-diffip04", true, true, true, true},
    // {"tcp4-multi-diffip05", true, true, true, true},
    // {"tcp4-multi-diffip06", true, true, true, true},
    // {"tcp4-multi-diffip07", true, true, true, true},
    // {"tcp4-multi-diffip08", true, true, true, true},
    // {"tcp4-multi-diffip09", true, true, true, true},
    // {"tcp4-multi-diffip10", true, true, true, true},
    // {"tcp4-multi-diffip11", true, true, true, true},
    // {"tcp4-multi-diffip12", true, true, true, true},
    // {"tcp4-multi-diffip13", true, true, true, true},
    // {"tcp4-multi-diffip14", true, true, true, true},
    // {"tcp4-multi-diffnic01", true, true, true, true},
    // {"tcp4-multi-diffnic02", true, true, true, true},
    // {"tcp4-multi-diffnic03", true, true, true, true},
    // {"tcp4-multi-diffnic04", true, true, true, true},
    // {"tcp4-multi-diffnic05", true, true, true, true},
    // {"tcp4-multi-diffnic06", true, true, true, true},
    // {"tcp4-multi-diffnic07", true, true, true, true},
    // {"tcp4-multi-diffnic08", true, true, true, true},
    // {"tcp4-multi-diffnic09", true, true, true, true},
    // {"tcp4-multi-diffnic10", true, true, true, true},
    // {"tcp4-multi-diffnic11", true, true, true, true},
    // {"tcp4-multi-diffnic12", true, true, true, true},
    // {"tcp4-multi-diffnic13", true, true, true, true},
    // {"tcp4-multi-diffnic14", true, true, true, true},
    // {"tcp4-multi-diffport01", true, true, true, true},
    // {"tcp4-multi-diffport02", true, true, true, true},
    // {"tcp4-multi-diffport03", true, true, true, true},
    // {"tcp4-multi-diffport04", true, true, true, true},
    // {"tcp4-multi-diffport05", true, true, true, true},
    // {"tcp4-multi-diffport06", true, true, true, true},
    // {"tcp4-multi-diffport07", true, true, true, true},
    // {"tcp4-multi-diffport08", true, true, true, true},
    // {"tcp4-multi-diffport09", true, true, true, true},
    // {"tcp4-multi-diffport10", true, true, true, true},
    // {"tcp4-multi-diffport11", true, true, true, true},
    // {"tcp4-multi-diffport12", true, true, true, true},
    // {"tcp4-multi-diffport13", true, true, true, true},
    // {"tcp4-multi-diffport14", true, true, true, true},
    // {"tcp4-multi-sameport01", true, true, true, true},
    // {"tcp4-multi-sameport02", true, true, true, true},
    // {"tcp4-multi-sameport03", true, true, true, true},
    // {"tcp4-multi-sameport04", true, true, true, true},
    // {"tcp4-multi-sameport05", true, true, true, true},
    // {"tcp4-multi-sameport06", true, true, true, true},
    // {"tcp4-multi-sameport07", true, true, true, true},
    // {"tcp4-multi-sameport08", true, true, true, true},
    // {"tcp4-multi-sameport09", true, true, true, true},
    // {"tcp4-multi-sameport10", true, true, true, true},
    // {"tcp4-multi-sameport11", true, true, true, true},
    // {"tcp4-multi-sameport12", true, true, true, true},
    // {"tcp4-multi-sameport13", true, true, true, true},
    // {"tcp4-multi-sameport14", true, true, true, true},
    // {"tcp4-uni-basic01", true, true, true, true},
    // {"tcp4-uni-basic02", true, true, true, true},
    // {"tcp4-uni-basic03", true, true, true, true},
    // {"tcp4-uni-basic04", true, true, true, true},
    // {"tcp4-uni-basic05", true, true, true, true},
    // {"tcp4-uni-basic06", true, true, true, true},
    // {"tcp4-uni-basic07", true, true, true, true},
    // {"tcp4-uni-basic08", true, true, true, true},
    // {"tcp4-uni-basic09", true, true, true, true},
    // {"tcp4-uni-basic10", true, true, true, true},
    // {"tcp4-uni-basic11", true, true, true, true},
    // {"tcp4-uni-basic12", true, true, true, true},
    // {"tcp4-uni-basic13", true, true, true, true},
    // {"tcp4-uni-basic14", true, true, true, true},
    // {"tcp4-uni-dsackoff01", true, true, true, true},
    // {"tcp4-uni-dsackoff02", true, true, true, true},
    // {"tcp4-uni-dsackoff03", true, true, true, true},
    // {"tcp4-uni-dsackoff04", true, true, true, true},
    // {"tcp4-uni-dsackoff05", true, true, true, true},
    // {"tcp4-uni-dsackoff06", true, true, true, true},
    // {"tcp4-uni-dsackoff07", true, true, true, true},
    // {"tcp4-uni-dsackoff08", true, true, true, true},
    // {"tcp4-uni-dsackoff09", true, true, true, true},
    // {"tcp4-uni-dsackoff10", true, true, true, true},
    // {"tcp4-uni-dsackoff11", true, true, true, true},
    // {"tcp4-uni-dsackoff12", true, true, true, true},
    // {"tcp4-uni-dsackoff13", true, true, true, true},
    // {"tcp4-uni-dsackoff14", true, true, true, true},
    // {"tcp4-uni-pktlossdup01", true, true, true, true},
    // {"tcp4-uni-pktlossdup02", true, true, true, true},
    // {"tcp4-uni-pktlossdup03", true, true, true, true},
    // {"tcp4-uni-pktlossdup04", true, true, true, true},
    // {"tcp4-uni-pktlossdup05", true, true, true, true},
    // {"tcp4-uni-pktlossdup06", true, true, true, true},
    // {"tcp4-uni-pktlossdup07", true, true, true, true},
    // {"tcp4-uni-pktlossdup08", true, true, true, true},
    // {"tcp4-uni-pktlossdup09", true, true, true, true},
    // {"tcp4-uni-pktlossdup10", true, true, true, true},
    // {"tcp4-uni-pktlossdup11", true, true, true, true},
    // {"tcp4-uni-pktlossdup12", true, true, true, true},
    // {"tcp4-uni-pktlossdup13", true, true, true, true},
    // {"tcp4-uni-pktlossdup14", true, true, true, true},
    // {"tcp4-uni-sackoff01", true, true, true, true},
    // {"tcp4-uni-sackoff02", true, true, true, true},
    // {"tcp4-uni-sackoff03", true, true, true, true},
    // {"tcp4-uni-sackoff04", true, true, true, true},
    // {"tcp4-uni-sackoff05", true, true, true, true},
    // {"tcp4-uni-sackoff06", true, true, true, true},
    // {"tcp4-uni-sackoff07", true, true, true, true},
    // {"tcp4-uni-sackoff08", true, true, true, true},
    // {"tcp4-uni-sackoff09", true, true, true, true},
    // {"tcp4-uni-sackoff10", true, true, true, true},
    // {"tcp4-uni-sackoff11", true, true, true, true},
    // {"tcp4-uni-sackoff12", true, true, true, true},
    // {"tcp4-uni-sackoff13", true, true, true, true},
    // {"tcp4-uni-sackoff14", true, true, true, true},
    // {"tcp4-uni-smallsend01", true, true, true, true},
    // {"tcp4-uni-smallsend02", true, true, true, true},
    // {"tcp4-uni-smallsend03", true, true, true, true},
    // {"tcp4-uni-smallsend04", true, true, true, true},
    // {"tcp4-uni-smallsend05", true, true, true, true},
    // {"tcp4-uni-smallsend06", true, true, true, true},
    // {"tcp4-uni-smallsend07", true, true, true, true},
    // {"tcp4-uni-smallsend08", true, true, true, true},
    // {"tcp4-uni-smallsend09", true, true, true, true},
    // {"tcp4-uni-smallsend10", true, true, true, true},
    // {"tcp4-uni-smallsend11", true, true, true, true},
    // {"tcp4-uni-smallsend12", true, true, true, true},
    // {"tcp4-uni-smallsend13", true, true, true, true},
    // {"tcp4-uni-smallsend14", true, true, true, true},
    // {"tcp4-uni-tso01", true, true, true, true},
    // {"tcp4-uni-tso02", true, true, true, true},
    // {"tcp4-uni-tso03", true, true, true, true},
    // {"tcp4-uni-tso04", true, true, true, true},
    // {"tcp4-uni-tso05", true, true, true, true},
    // {"tcp4-uni-tso06", true, true, true, true},
    // {"tcp4-uni-tso07", true, true, true, true},
    // {"tcp4-uni-tso08", true, true, true, true},
    // {"tcp4-uni-tso09", true, true, true, true},
    // {"tcp4-uni-tso10", true, true, true, true},
    // {"tcp4-uni-tso11", true, true, true, true},
    // {"tcp4-uni-tso12", true, true, true, true},
    // {"tcp4-uni-tso13", true, true, true, true},
    // {"tcp4-uni-tso14", true, true, true, true},
    // {"tcp4-uni-winscale01", true, true, true, true},
    // {"tcp4-uni-winscale02", true, true, true, true},
    // {"tcp4-uni-winscale03", true, true, true, true},
    // {"tcp4-uni-winscale04", true, true, true, true},
    // {"tcp4-uni-winscale05", true, true, true, true},
    // {"tcp4-uni-winscale06", true, true, true, true},
    // {"tcp4-uni-winscale07", true, true, true, true},
    // {"tcp4-uni-winscale08", true, true, true, true},
    // {"tcp4-uni-winscale09", true, true, true, true},
    // {"tcp4-uni-winscale10", true, true, true, true},
    // {"tcp4-uni-winscale11", true, true, true, true},
    // {"tcp4-uni-winscale12", true, true, true, true},
    // {"tcp4-uni-winscale13", true, true, true, true},
    // {"tcp4-uni-winscale14", true, true, true, true},
    // {"tcp6-multi-diffip01", true, true, true, true},
    // {"tcp6-multi-diffip02", true, true, true, true},
    // {"tcp6-multi-diffip03", true, true, true, true},
    // {"tcp6-multi-diffip04", true, true, true, true},
    // {"tcp6-multi-diffip05", true, true, true, true},
    // {"tcp6-multi-diffip06", true, true, true, true},
    // {"tcp6-multi-diffip07", true, true, true, true},
    // {"tcp6-multi-diffip08", true, true, true, true},
    // {"tcp6-multi-diffip09", true, true, true, true},
    // {"tcp6-multi-diffip10", true, true, true, true},
    // {"tcp6-multi-diffip11", true, true, true, true},
    // {"tcp6-multi-diffip12", true, true, true, true},
    // {"tcp6-multi-diffip13", true, true, true, true},
    // {"tcp6-multi-diffip14", true, true, true, true},
    // {"tcp6-multi-diffnic01", true, true, true, true},
    // {"tcp6-multi-diffnic02", true, true, true, true},
    // {"tcp6-multi-diffnic03", true, true, true, true},
    // {"tcp6-multi-diffnic04", true, true, true, true},
    // {"tcp6-multi-diffnic05", true, true, true, true},
    // {"tcp6-multi-diffnic06", true, true, true, true},
    // {"tcp6-multi-diffnic07", true, true, true, true},
    // {"tcp6-multi-diffnic08", true, true, true, true},
    // {"tcp6-multi-diffnic09", true, true, true, true},
    // {"tcp6-multi-diffnic10", true, true, true, true},
    // {"tcp6-multi-diffnic11", true, true, true, true},
    // {"tcp6-multi-diffnic12", true, true, true, true},
    // {"tcp6-multi-diffnic13", true, true, true, true},
    // {"tcp6-multi-diffnic14", true, true, true, true},
    // {"tcp6-multi-diffport01", true, true, true, true},
    // {"tcp6-multi-diffport02", true, true, true, true},
    // {"tcp6-multi-diffport03", true, true, true, true},
    // {"tcp6-multi-diffport04", true, true, true, true},
    // {"tcp6-multi-diffport05", true, true, true, true},
    // {"tcp6-multi-diffport06", true, true, true, true},
    // {"tcp6-multi-diffport07", true, true, true, true},
    // {"tcp6-multi-diffport08", true, true, true, true},
    // {"tcp6-multi-diffport09", true, true, true, true},
    // {"tcp6-multi-diffport10", true, true, true, true},
    // {"tcp6-multi-diffport11", true, true, true, true},
    // {"tcp6-multi-diffport12", true, true, true, true},
    // {"tcp6-multi-diffport13", true, true, true, true},
    // {"tcp6-multi-diffport14", true, true, true, true},
    // {"tcp6-multi-sameport01", true, true, true, true},
    // {"tcp6-multi-sameport02", true, true, true, true},
    // {"tcp6-multi-sameport03", true, true, true, true},
    // {"tcp6-multi-sameport04", true, true, true, true},
    // {"tcp6-multi-sameport05", true, true, true, true},
    // {"tcp6-multi-sameport06", true, true, true, true},
    // {"tcp6-multi-sameport07", true, true, true, true},
    // {"tcp6-multi-sameport08", true, true, true, true},
    // {"tcp6-multi-sameport09", true, true, true, true},
    // {"tcp6-multi-sameport10", true, true, true, true},
    // {"tcp6-multi-sameport11", true, true, true, true},
    // {"tcp6-multi-sameport12", true, true, true, true},
    // {"tcp6-multi-sameport13", true, true, true, true},
    // {"tcp6-multi-sameport14", true, true, true, true},
    // {"tcp6-uni-basic01", true, true, true, true},
    // {"tcp6-uni-basic02", true, true, true, true},
    // {"tcp6-uni-basic03", true, true, true, true},
    // {"tcp6-uni-basic04", true, true, true, true},
    // {"tcp6-uni-basic05", true, true, true, true},
    // {"tcp6-uni-basic06", true, true, true, true},
    // {"tcp6-uni-basic07", true, true, true, true},
    // {"tcp6-uni-basic08", true, true, true, true},
    // {"tcp6-uni-basic09", true, true, true, true},
    // {"tcp6-uni-basic10", true, true, true, true},
    // {"tcp6-uni-basic11", true, true, true, true},
    // {"tcp6-uni-basic12", true, true, true, true},
    // {"tcp6-uni-basic13", true, true, true, true},
    // {"tcp6-uni-basic14", true, true, true, true},
    // {"tcp6-uni-dsackoff01", true, true, true, true},
    // {"tcp6-uni-dsackoff02", true, true, true, true},
    // {"tcp6-uni-dsackoff03", true, true, true, true},
    // {"tcp6-uni-dsackoff04", true, true, true, true},
    // {"tcp6-uni-dsackoff05", true, true, true, true},
    // {"tcp6-uni-dsackoff06", true, true, true, true},
    // {"tcp6-uni-dsackoff07", true, true, true, true},
    // {"tcp6-uni-dsackoff08", true, true, true, true},
    // {"tcp6-uni-dsackoff09", true, true, true, true},
    // {"tcp6-uni-dsackoff10", true, true, true, true},
    // {"tcp6-uni-dsackoff11", true, true, true, true},
    // {"tcp6-uni-dsackoff12", true, true, true, true},
    // {"tcp6-uni-dsackoff13", true, true, true, true},
    // {"tcp6-uni-dsackoff14", true, true, true, true},
    // {"tcp6-uni-pktlossdup01", true, true, true, true},
    // {"tcp6-uni-pktlossdup02", true, true, true, true},
    // {"tcp6-uni-pktlossdup03", true, true, true, true},
    // {"tcp6-uni-pktlossdup04", true, true, true, true},
    // {"tcp6-uni-pktlossdup05", true, true, true, true},
    // {"tcp6-uni-pktlossdup06", true, true, true, true},
    // {"tcp6-uni-pktlossdup07", true, true, true, true},
    // {"tcp6-uni-pktlossdup08", true, true, true, true},
    // {"tcp6-uni-pktlossdup09", true, true, true, true},
    // {"tcp6-uni-pktlossdup10", true, true, true, true},
    // {"tcp6-uni-pktlossdup11", true, true, true, true},
    // {"tcp6-uni-pktlossdup12", true, true, true, true},
    // {"tcp6-uni-pktlossdup13", true, true, true, true},
    // {"tcp6-uni-pktlossdup14", true, true, true, true},
    // {"tcp6-uni-sackoff01", true, true, true, true},
    // {"tcp6-uni-sackoff02", true, true, true, true},
    // {"tcp6-uni-sackoff03", true, true, true, true},
    // {"tcp6-uni-sackoff04", true, true, true, true},
    // {"tcp6-uni-sackoff05", true, true, true, true},
    // {"tcp6-uni-sackoff06", true, true, true, true},
    // {"tcp6-uni-sackoff07", true, true, true, true},
    // {"tcp6-uni-sackoff08", true, true, true, true},
    // {"tcp6-uni-sackoff09", true, true, true, true},
    // {"tcp6-uni-sackoff10", true, true, true, true},
    // {"tcp6-uni-sackoff11", true, true, true, true},
    // {"tcp6-uni-sackoff12", true, true, true, true},
    // {"tcp6-uni-sackoff13", true, true, true, true},
    // {"tcp6-uni-sackoff14", true, true, true, true},
    // {"tcp6-uni-smallsend01", true, true, true, true},
    // {"tcp6-uni-smallsend02", true, true, true, true},
    // {"tcp6-uni-smallsend03", true, true, true, true},
    // {"tcp6-uni-smallsend04", true, true, true, true},
    // {"tcp6-uni-smallsend05", true, true, true, true},
    // {"tcp6-uni-smallsend06", true, true, true, true},
    // {"tcp6-uni-smallsend07", true, true, true, true},
    // {"tcp6-uni-smallsend08", true, true, true, true},
    // {"tcp6-uni-smallsend09", true, true, true, true},
    // {"tcp6-uni-smallsend10", true, true, true, true},
    // {"tcp6-uni-smallsend11", true, true, true, true},
    // {"tcp6-uni-smallsend12", true, true, true, true},
    // {"tcp6-uni-smallsend13", true, true, true, true},
    // {"tcp6-uni-smallsend14", true, true, true, true},
    // {"tcp6-uni-tso01", true, true, true, true},
    // {"tcp6-uni-tso02", true, true, true, true},
    // {"tcp6-uni-tso03", true, true, true, true},
    // {"tcp6-uni-tso04", true, true, true, true},
    // {"tcp6-uni-tso05", true, true, true, true},
    // {"tcp6-uni-tso06", true, true, true, true},
    // {"tcp6-uni-tso07", true, true, true, true},
    // {"tcp6-uni-tso08", true, true, true, true},
    // {"tcp6-uni-tso09", true, true, true, true},
    // {"tcp6-uni-tso10", true, true, true, true},
    // {"tcp6-uni-tso11", true, true, true, true},
    // {"tcp6-uni-tso12", true, true, true, true},
    // {"tcp6-uni-tso13", true, true, true, true},
    // {"tcp6-uni-tso14", true, true, true, true},
    // {"tcp6-uni-winscale01", true, true, true, true},
    // {"tcp6-uni-winscale02", true, true, true, true},
    // {"tcp6-uni-winscale03", true, true, true, true},
    // {"tcp6-uni-winscale04", true, true, true, true},
    // {"tcp6-uni-winscale05", true, true, true, true},
    // {"tcp6-uni-winscale06", true, true, true, true},
    // {"tcp6-uni-winscale07", true, true, true, true},
    // {"tcp6-uni-winscale08", true, true, true, true},
    // {"tcp6-uni-winscale09", true, true, true, true},
    // {"tcp6-uni-winscale10", true, true, true, true},
    // {"tcp6-uni-winscale11", true, true, true, true},
    // {"tcp6-uni-winscale12", true, true, true, true},
    // {"tcp6-uni-winscale13", true, true, true, true},
    // {"tcp6-uni-winscale14", true, true, true, true},
    // {"tee01", true, true, true, true},
    // {"tee02", true, true, true, true},
    // {"test.sh", true, true, true, true},
    // {"test_controllers.sh", true, true, true, true},
    // {"test_ioctl", true, true, true, true},
    // {"test_robind.sh", true, true, true, true},
    // {"testsf_c", true, true, true, true},
    // {"testsf_c6", true, true, true, true},
    // {"testsf_s", true, true, true, true},
    // {"testsf_s6", true, true, true, true},
    // {"tgkill02", true, true, true, true},
    // {"tgkill03", true, true, true, true},
    // {"thp01", true, true, true, true},
    // {"thp02", true, true, true, true},
    // {"thp03", true, true, true, true},
    // {"thp04", true, true, true, true},
    // {"timed_forkbomb", true, true, true, true},
    // {"timens01", true, true, true, true}, //.config
    // {"timer_delete01", true, true, true, true},
    // {"timer_delete02", true, true, true, true},
    // {"timer_getoverrun01", true, true, true, true},
    // {"timer_gettime01", true, true, true, true},
    // {"timer_settime03", true, true, true, true},
    // {"timerfd_gettime01", true, true, true, true},
    // {"timerfd_settime01", true, true, true, true},
    // {"timerfd_settime02", true, true, true, true},
    // {"timerfd01", true, true, true, true},
    // {"timerfd04", true, true, true, true},
    // {"time-schedule", true, true, true, true},
    // {"tpci", true, true, true, true},
    // {"tpm_changeauth_tests.sh", true, true, true, true},
    // {"tpm_changeauth_tests_exp01.sh", true, true, true, true},
    // {"tpm_changeauth_tests_exp02.sh", true, true, true, true},
    // {"tpm_changeauth_tests_exp03.sh", true, true, true, true},
    // {"tpm_clear_tests.sh", true, true, true, true},
    // {"tpm_clear_tests_exp01.sh", true, true, true, true},
    // {"tpm_getpubek_tests.sh", true, true, true, true},
    // {"tpm_getpubek_tests_exp01.sh", true, true, true, true},
    // {"tpm_restrictpubek_tests.sh", true, true, true, true},
    // {"tpm_restrictpubek_tests_exp01.sh", true, true, true, true},
    // {"tpm_restrictpubek_tests_exp02.sh", true, true, true, true},
    // {"tpm_restrictpubek_tests_exp03.sh", true, true, true, true},
    // {"tpm_selftest_tests.sh", true, true, true, true},
    // {"tpm_takeownership_tests.sh", true, true, true, true},
    // {"tpm_takeownership_tests_exp01.sh", true, true, true, true},
    // {"tpm_version_tests.sh", true, true, true, true},
    // {"tpmtoken_import_tests.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp01.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp02.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp03.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp04.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp05.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp06.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp07.sh", true, true, true, true},
    // {"tpmtoken_import_tests_exp08.sh", true, true, true, true},
    // {"tpmtoken_init_tests.sh", true, true, true, true},
    // {"tpmtoken_init_tests_exp00.sh", true, true, true, true},
    // {"tpmtoken_init_tests_exp01.sh", true, true, true, true},
    // {"tpmtoken_init_tests_exp02.sh", true, true, true, true},
    // {"tpmtoken_init_tests_exp03.sh", true, true, true, true},
    // {"tpmtoken_objects_tests.sh", true, true, true, true},
    // {"tpmtoken_objects_tests_exp01.sh", true, true, true, true},
    // {"tpmtoken_protect_tests.sh", true, true, true, true},
    // {"tpmtoken_protect_tests_exp01.sh", true, true, true, true},
    // {"tpmtoken_protect_tests_exp02.sh", true, true, true, true},
    // {"tpmtoken_setpasswd_tests.sh", true, true, true, true},
    // {"tpmtoken_setpasswd_tests_exp01.sh", true, true, true, true},
    // {"tpmtoken_setpasswd_tests_exp02.sh", true, true, true, true},
    // {"tpmtoken_setpasswd_tests_exp03.sh", true, true, true, true},
    // {"tpmtoken_setpasswd_tests_exp04.sh", true, true, true, true},
    // {"trace_sched", true, true, true, true},
    // {"tst_ansi_color.sh", true, true, true, true},
    // {"tst_brk", true, true, true, true},
    // {"tst_brkm", true, true, true, true},
    // {"tst_cgctl", true, true, true, true},
    // {"tst_check_drivers", true, true, true, true},
    // {"tst_check_kconfigs", true, true, true, true},
    // {"tst_checkpoint", true, true, true, true},
    // {"tst_device", true, true, true, true},
    // {"tst_exit", true, true, true, true},
    // {"tst_fs_has_free", true, true, true, true},
    // {"tst_fsfreeze", true, true, true, true},
    // {"tst_get_free_pids", true, true, true, true},
    // {"tst_get_median", true, true, true, true},
    // {"tst_get_unused_port", true, true, true, true},
    // {"tst_getconf", true, true, true, true},
    // {"tst_hexdump", true, true, true, true},
    // {"tst_kvcmp", true, true, true, true},
    // {"tst_lockdown_enabled", true, true, true, true},
    // {"tst_ncpus", true, true, true, true},
    // {"tst_ncpus_conf", true, true, true, true},
    // {"tst_ncpus_max", true, true, true, true},
    // {"tst_net.sh", true, true, true, true},
    // {"tst_net_iface_prefix", true, true, true, true},
    // {"tst_net_ip_prefix", true, true, true, true},
    // {"tst_net_stress.sh", true, true, true, true},
    // {"tst_net_vars", true, true, true, true},
    // {"tst_ns_create", true, true, true, true},
    // {"tst_ns_exec", true, true, true, true},
    // {"tst_ns_ifmove", true, true, true, true},
    // {"tst_random", true, true, true, true},
    // {"tst_res", true, true, true, true},
    // {"tst_resm", true, true, true, true},
    // {"tst_rod", true, true, true, true},
    // {"tst_secureboot_enabled", true, true, true, true},
    // {"tst_security.sh", true, true, true, true},
    // {"tst_sleep", true, true, true, true},
    // {"tst_supported_fs", true, true, true, true},
    // {"tst_test.sh", true, true, true, true},
    // {"tst_timeout_kill", true, true, true, true},
    // {"uaccess", true, true, true, true},
    // {"udp4-multi-diffip01", true, true, true, true},
    // {"udp4-multi-diffip02", true, true, true, true},
    // {"udp4-multi-diffip03", true, true, true, true},
    // {"udp4-multi-diffip04", true, true, true, true},
    // {"udp4-multi-diffip05", true, true, true, true},
    // {"udp4-multi-diffip06", true, true, true, true},
    // {"udp4-multi-diffip07", true, true, true, true},
    // {"udp4-multi-diffnic01", true, true, true, true},
    // {"udp4-multi-diffnic02", true, true, true, true},
    // {"udp4-multi-diffnic03", true, true, true, true},
    // {"udp4-multi-diffnic04", true, true, true, true},
    // {"udp4-multi-diffnic05", true, true, true, true},
    // {"udp4-multi-diffnic06", true, true, true, true},
    // {"udp4-multi-diffnic07", true, true, true, true},
    // {"udp4-multi-diffport01", true, true, true, true},
    // {"udp4-multi-diffport02", true, true, true, true},
    // {"udp4-multi-diffport03", true, true, true, true},
    // {"udp4-multi-diffport04", true, true, true, true},
    // {"udp4-multi-diffport05", true, true, true, true},
    // {"udp4-multi-diffport06", true, true, true, true},
    // {"udp4-multi-diffport07", true, true, true, true},
    // {"udp4-uni-basic01", true, true, true, true},
    // {"udp4-uni-basic02", true, true, true, true},
    // {"udp4-uni-basic03", true, true, true, true},
    // {"udp4-uni-basic04", true, true, true, true},
    // {"udp4-uni-basic05", true, true, true, true},
    // {"udp4-uni-basic06", true, true, true, true},
    // {"udp4-uni-basic07", true, true, true, true},
    // {"udp6-multi-diffip01", true, true, true, true},
    // {"udp6-multi-diffip02", true, true, true, true},
    // {"udp6-multi-diffip03", true, true, true, true},
    // {"udp6-multi-diffip04", true, true, true, true},
    // {"udp6-multi-diffip05", true, true, true, true},
    // {"udp6-multi-diffip06", true, true, true, true},
    // {"udp6-multi-diffip07", true, true, true, true},
    // {"udp6-multi-diffnic01", true, true, true, true},
    // {"udp6-multi-diffnic02", true, true, true, true},
    // {"udp6-multi-diffnic03", true, true, true, true},
    // {"udp6-multi-diffnic04", true, true, true, true},
    // {"udp6-multi-diffnic05", true, true, true, true},
    // {"udp6-multi-diffnic06", true, true, true, true},
    // {"udp6-multi-diffnic07", true, true, true, true},
    // {"udp6-multi-diffport01", true, true, true, true},
    // {"udp6-multi-diffport02", true, true, true, true},
    // {"udp6-multi-diffport03", true, true, true, true},
    // {"udp6-multi-diffport04", true, true, true, true},
    // {"udp6-multi-diffport05", true, true, true, true},
    // {"udp6-multi-diffport06", true, true, true, true},
    // {"udp6-multi-diffport07", true, true, true, true},
    // {"udp6-uni-basic01", true, true, true, true},
    // {"udp6-uni-basic02", true, true, true, true},
    // {"udp6-uni-basic03", true, true, true, true},
    // {"udp6-uni-basic04", true, true, true, true},
    // {"udp6-uni-basic05", true, true, true, true},
    // {"udp6-uni-basic06", true, true, true, true},
    // {"udp6-uni-basic07", true, true, true, true},
    // {"uevent01", true, true, true, true},
    // {"uevent02", true, true, true, true},
    // {"uevent03", true, true, true, true},
    // {"umask01", true, true, true, true},
    // {"umip_basic_test", true, true, true, true},
    // {"umount01", true, true, true, true},
    // {"umount02", true, true, true, true},
    // {"umount03", true, true, true, true},
    // {"umount2_01", true, true, true, true},
    // {"umount2_02", true, true, true, true},
    // {"unshare01", true, true, true, true},//fail3 TFAIL: unshare(CLONE_FILES) failed: ENOSYS (38)
    // {"unshare01.sh", true, true, true, true},
    // {"unshare02", true, true, true, true},//fail2 TFAIL: unshare(CLONE_NEWNS) expected EPERM: ENOSYS (38)
    // {"unzip01.sh", true, true, true, true},
    // {"userfaultfd01", true, true, true, true},//broken 1 TBROK: ioctl(3,((((2U|1U) << (((0+8)+8)+14)) | (((0xAA)) << (0+8)) | ((((0x3F))) << 0) | ((((sizeof(struct uffdio_api)))) << ((0+8)+8)))),...) failed: ENOTTY (25)
    // {"userns01", true, true, true, true},//TCONF
    // {"userns02", true, true, true, true},//TCONF
    // {"userns03", true, true, true, true},//TCONF 
    // {"userns04", true, true, true, true},//TCONF 
    // {"userns05", true, true, true, true},//TCONF 
    // {"userns06", true, true, true, true},//TCONF 
    // {"userns06_capcheck", true, true, true, true},// TBROK: LTP_IPC_PATH is not defined
    // {"userns07", true, true, true, true},//TCONF
    // {"userns08", true, true, true, true},//TCONF
    // {"ustat01", true, true, true, true},//TCONF
    // {"ustat02", true, true, true, true},//TCONF
    // {"utime05", true, true, true, true},//pass3
    // {"utime06", true, true, true, true},//pass4
    // {"utime07", true, true, true, true},//pass3 fail2
    // {"utimes01", true, true, true, true},//pass6 fail1
    // {"utsname01", true, true, true, true},//pass1
    // {"utsname02", true, true, true, true},//panic: kernel/sys/syscall_handler.cc:17525: 未实现该系统调用
    // {"utsname03", true, true, true, true},//panic: kernel/sys/syscall_handler.cc:17525: 未实现该系统调用
    // {"utsname04", true, true, true, true},//pass2 fail2
    // {"verify_caps_exec", true, true, true, true},//TCONF
    // {"vfork", true, true, true, true},//TCONF
    // {"vfork_freeze.sh", true, true, true, true},
    // {"vfork01", true, true, true, true},//TFAIL:Device/inode number of parent and childs '/'  don't match
    // {"vfork02", true, true, true, true},//TPASS
    // {"vhangup01", true, true, true, true},//TCONF: syscall(58) __NR_vhangup not supported on your arch
    // {"vhangup02", true, true, true, true},//TCONF
    // {"virt_lib.sh", true, true, true, true},
    // {"vma01", true, true, true, true}, //TFAIL  :  vma01.c:190: A single 6*ps VMA found.
    // {"vma02", true, true, true, true},//TCONF
    // {"vma03", true, true, true, true},//TCONF
    // {"vma04", true, true, true, true},//TCONF
    // {"vma05.sh", true, true, true, true},
    // {"vma05_vdso", true, true, true, true},//fail
    // {"vmsplice01", true, true, true, true},//TBROK: read(3,0x187970,131072) failed, returned 0: ENOENT (2)
    // {"vmsplice02", true, true, true, true},//fail3
    // {"vmsplice03", true, true, true, true},//fail1  TFAIL: vmsplice() didn't write anything
    // {"vmsplice04", true, true, true, true},//fail2 
    // {"wait403", true, true, true, true}, // PASS
    // {"waitid07", true, true, true, true},//PASS
    // {"waitid08", true, true, true, true},//pass
    // {"waitid09", true, true, true, true},//pass
    // {"waitid10", true, true, true, true},//TBROK: Failed to open FILE '/proc/sys/kernel/core_pattern' for reading: ENOENT (2)
    // {"waitpid10", true, true, true, true},//pass
    // {"waitpid11", true, true, true, true},//pass
    // {"waitpid12", true, true, true, true},//pass
    // {"waitpid13", true, true, true, true},//pass
    // {"wc01.sh", true, true, true, true},
    // {"which01.sh", true, true, true, true}, 
    // {"wireguard_lib.sh", true, true, true, true},
    // {"wqueue01", true, true, true, true},//TCONF
    // {"wqueue02", true, true, true, true},//TCONF
    // {"wqueue03", true, true, true, true},//TCONF
    // {"wqueue04", true, true, true, true},//TCONF
    // {"wqueue05", true, true, true, true},//TCONF
    // {"wqueue06", true, true, true, true},//TCONF
    // {"wqueue07", true, true, true, true},//TCONF
    // {"wqueue08", true, true, true, true},//TCONF
    // {"wqueue09", true, true, true, true},//TCONF
    // {"write_freezing.sh", true, true, true, true},
    // {"write06", true, true, true, true},//pass
    // {"writetest", true, true, true, true},//FAIL LTP CASE writetest: -11
    // {"writev03", true, true, true, true},//TCONF
    // {"zram_lib.sh", true, true, true, true},
    // {"zram01.sh", true, true, true, true},
    // {"zram02.sh", true, true, true, true},
    // {"zram03", true, true, true, true},//TCONF

};
int basic_musl_test(void)
{
    return basic_test(musl_dir);
}

int basic_glibc_test(void)
{
    return basic_test(glibc_dir);
}

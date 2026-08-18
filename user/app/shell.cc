#include "user.hh"
#include "fuckyou.hh"

// Alpine rootfs 使用 /bin/busybox，评测 sdcard 把 BusyBox 放在
// /musl/busybox，而 selfhost Debian 只提供 bash/dash。shell 调试入口必须
// 同时支持这三类镜像，才能直接启动真实 stress-ng 与 Cargo 环境。
static const char *g_shell_binary = "/bin/busybox";
static const char *g_shell_argv0 = "sh";

bool path_exists(const char *path)
{
    int fd = openat(AT_FDCWD, path, O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    close(fd);
    return true;
}

static int decode_wait_status(int raw_status)
{
    if ((raw_status & 0x7f) == 0)
    {
        return (raw_status >> 8) & 0xff;
    }
    return -(raw_status & 0x7f);
}

static int run_foreground(const char *path, char *argv[], char *envp[], const char *cwd = 0)
{
    int pid = fork();
    if (pid < 0)
    {
        printf("[shell] fork 失败: %s\n", path);
        return -1;
    }

    if (pid == 0)
    {
        if (cwd != 0 && chdir(cwd) != 0)
        {
            printf("[shell] chdir 失败: %s\n", cwd);
            exit(126);
            return 126;
        }
        execve(path, argv, envp);
        printf("[shell] execve 失败: %s\n", path);
        exit(127);
        return 127;
    }

    int raw_status = -1;
    if (waitpid(pid, &raw_status, 0) < 0)
    {
        printf("[shell] waitpid 失败: %s\n", path);
        return -1;
    }
    return decode_wait_status(raw_status);
}

static void enter_shell_workdir()
{
    // rootfs 镜像是标准 Linux 目录布局，优先把交互式 shell 放到 /root，
    // 这样宿主机挂载看到的家目录内容和 guest 内的初始 cwd 保持一致。
    if (chdir("/root") == 0)
    {
        return;
    }
    if (chdir("/") != 0)
    {
        printf("[shell] chdir(/) 失败\n");
    }
}

static bool init_shell_environment()
{
    // shell 模式现在直接挂载 rootfs，不能再假设 /musl /glibc /fat32 这些评测盘目录存在。
    // 这里在进入 shell 前做最小存在性校验，便于快速判断镜像是否挂对。
    if (path_exists("/bin/busybox"))
    {
        g_shell_binary = "/bin/busybox";
    }
    else if (path_exists("/musl/busybox"))
    {
        g_shell_binary = "/musl/busybox";
    }
    else if (path_exists("/bin/bash"))
    {
        g_shell_binary = "/bin/bash";
        g_shell_argv0 = "bash";
    }
    else if (path_exists("/bin/sh"))
    {
        g_shell_binary = "/bin/sh";
        g_shell_argv0 = "sh";
    }
    else
    {
        printf("[shell] 缺少 BusyBox、/bin/bash 和 /bin/sh，当前根文件系统无法交互\n");
        return false;
    }
    enter_shell_workdir();
    return true;
}

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        char *envp[] = {
            // rootfs 使用标准 FHS 目录，PATH/库路径也按常规 Linux 布局设置。
            (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/musl",
            (char *)"LD_LIBRARY_PATH=/lib:/usr/lib",
            (char *)"HOME=/root",
            (char *)"PWD=/root",
            (char *)"OLDPWD=/root",
            (char *)"TERM=vt100",
            (char *)"USER=root",
            (char *)"LOGNAME=root",
            (char *)"SHELL=/bin/sh",
            // BusyBox ash 支持 \w 展示当前工作目录，这里直接把 cwd 放进提示符里。
            (char *)"PS1=F7LY:\\w$ ",
            0,
        };
        if (!init_shell_environment())
        {
            print_f7ly();
            printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
            printf("[shell] shell 初始化失败，准备关机\n");
            printfMagenta("#### F7LY INTERACTIVE SHELL END ret=127 ####\n");
            print_fuckyou();
            shutdown();
            return 127;
        }
        print_f7ly();
        printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
        printfMagenta("type \"exit\" to quit\n");
        char *shell_argv[] = {
            (char *)g_shell_argv0,
            (char *)"-i",
            0,
        };
        // init_shell_environment() 已经选择了 /root 或 / 作为当前目录；子进程直接
        // 继承即可。评测 sdcard 没有 /root，不能在这里再次强制 chdir("/root")。
        int shell_ret = run_foreground(g_shell_binary, shell_argv, envp);
        printfMagenta("#### F7LY INTERACTIVE SHELL END ret=%d ####\n", shell_ret);
        print_goodbye();

        shutdown();
        return 0;
    }
}

#include "user.hh"
#include "fuckyou.hh"

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

static void init_shell_environment(char *envp[])
{
    // 交互式 shell 也沿用回归入口的 /bin 约定，但这里不用依赖 user_test.cc 的构建链路，
    // 直接把 BusyBox applet 安装和 shell 启动目录显式收拢到一个初始化函数里。
    int mkdir_ret = mkdir("/bin", 0777);
    if (mkdir_ret != 0 && mkdir_ret != -17)
    {
        printf("[shell] mkdir(/bin) 失败: %d\n", mkdir_ret);
    }

    char *install_argv[] = {
        (char *)"busybox",
        (char *)"--install",
        (char *)"/bin",
        0,
    };
    int install_ret = run_foreground("/musl/busybox", install_argv, envp, "/musl/");
    if (install_ret != 0)
    {
        printf("[shell] busybox --install 返回 %d，继续启动 ash\n", install_ret);
    }

    if (chdir("/") != 0)
    {
        printf("[shell] chdir(/) 失败\n");
    }
}

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        char *envp[] = {
            (char *)"PATH=/bin:/musl:/glibc:/fat32/bin:/fat32/usr/bin",
            (char *)"LD_LIBRARY_PATH=/musl/lib:/glibc/lib",
            (char *)"HOME=/",
            (char *)"PWD=/",
            (char *)"OLDPWD=/",
            (char *)"TERM=vt100",
            (char *)"USER=root",
            (char *)"LOGNAME=root",
            (char *)"SHELL=/bin/sh",
            // BusyBox ash 支持 \w 展示当前工作目录，这里直接把 cwd 放进提示符里。
            (char *)"PS1=F7LY:\\w$ ",
            0,
        };
        init_shell_environment(envp);
        print_f7ly();
        printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
        printfMagenta("type \"exit\" to quit\n");
        char *shell_argv[] = {
            (char *)"busybox",
            (char *)"ash",
            0,
        };
        int shell_ret = run_foreground("/musl/busybox", shell_argv, envp, "/");
        printfMagenta("#### F7LY INTERACTIVE SHELL END ret=%d ####\n", shell_ret);
        print_fuckyou();
        
        shutdown();
        return 0;
    }
}

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

static int run_foreground(const char *path, char *argv[], char *envp[])
{
    int pid = fork();
    if (pid < 0)
    {
        printf("[shell] fork 失败: %s\n", path);
        return -1;
    }

    if (pid == 0)
    {
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

static void prepare_busybox_applets(char *envp[])
{
    // ash 交互时会通过 PATH 找 ls/cat 等 applet，沿用回归入口的 /bin 安装约定。
    int mkdir_ret = mkdir("/bin", 0777);
    if (mkdir_ret != 0 && mkdir_ret != -17)
    {
        printf("[shell] mkdir(/bin) 失败: %d\n", mkdir_ret);
    }

    if (chdir("/musl/") != 0)
    {
        printf("[shell] chdir(/musl/) 失败，仍会尝试直接启动 /musl/busybox\n");
        return;
    }

    char *install_argv[] = {
        (char *)"busybox",
        (char *)"--install",
        (char *)"/bin",
        0,
    };
    int install_ret = run_foreground("busybox", install_argv, envp);
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
            (char *)"PATH=/bin:/musl:/glibc",
            (char *)"LD_LIBRARY_PATH=/musl/lib:/glibc/lib",
            (char *)"HOME=/",
            (char *)"TERM=vt100",
            (char *)"PS1=F7LY$ ",
            0,
        };
        prepare_busybox_applets(envp);
        // print_f7ly();
        print_fuckyou();
        printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
        char *shell_argv[] = {
            (char *)"busybox",
            (char *)"ash",
            0,
        };
        int shell_ret = run_foreground("/musl/busybox", shell_argv, envp);
        printf("#### F7LY INTERACTIVE SHELL END ret=%d ####\n", shell_ret);

        shutdown();
        return 0;
    }
}

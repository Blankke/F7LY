#include "user.hh"
#include "fuckyou.hh"

static bool path_exists(const char *path)
{
    int fd = openat(AT_FDCWD, path, O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    close(fd);
    return true;
}

static const char *enter_shell_workdir()
{
    int ret = chdir("/root");
    if (ret == 0)
    {
        return "/root";
    }

    printf("[shell] chdir(/root) failed: %d, fallback to /\n", ret);
    ret = chdir("/");
    if (ret == 0)
    {
        return "/";
    }

    printf("[shell] chdir(/) failed: %d\n", ret);
    return 0;
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

static bool init_shell_environment(const char *&workdir)
{
    // shell 模式现在直接挂载 rootfs，不能再假设 /musl /glibc /fat32 这些评测盘目录存在。
    // 这里在进入 shell 前做最小存在性校验，便于快速判断镜像是否挂对。
    if (!path_exists("/bin/busybox"))
    {
        printf("[shell] 缺少 /bin/busybox，当前根文件系统不像是可交互 rootfs\n");
        return false;
    }

    workdir = enter_shell_workdir();
    return workdir != 0;
}

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        const char *workdir = 0;
        if (!init_shell_environment(workdir))
        {
            print_f7ly();
            printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
            printf("[shell] shell 初始化失败，准备关机\n");
            printfMagenta("#### F7LY INTERACTIVE SHELL END ret=127 ####\n");
            print_fuckyou();
            shutdown();
            return 127;
        }

        char *pwd_env = (char *)(workdir[1] == '\0' ? "PWD=/" : "PWD=/root");
        char *oldpwd_env = (char *)(workdir[1] == '\0' ? "OLDPWD=/" : "OLDPWD=/root");
        char *envp[] = {
            // rootfs 使用标准 FHS 目录，PATH/库路径也按常规 Linux 布局设置。
            (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
            (char *)"LD_LIBRARY_PATH=/lib:/usr/lib",
            (char *)"HOME=/root",
            pwd_env,
            oldpwd_env,
            (char *)"TERM=vt100",
            (char *)"USER=root",
            (char *)"LOGNAME=root",
            (char *)"SHELL=/bin/sh",
            // BusyBox ash 支持 \w 展示当前工作目录，这里直接把 cwd 放进提示符里。
            (char *)"PS1=F7LY:\\w$ ",
            0,
        };
        printfMagenta("#### F7LY INTERACTIVE SHELL START ####\n");
        printfMagenta("type \"exit\" to quit\n");
        char *shell_argv[] = {
            (char *)"sh",
            (char *)"-i",
            0,
        };
        // rootfs 沿用标准 /bin/busybox 布局，交互式 shell 直接从这里进入。
        int shell_ret = run_foreground("/bin/busybox", shell_argv, envp);
        printfMagenta("#### F7LY INTERACTIVE SHELL END ret=%d ####\n", shell_ret);
        print_goodbye();

        shutdown();
        return 0;
    }
}

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

static int run_busybox(char *argv[], char *envp[], const char *cwd = "/")
{
    return run_foreground("/musl/busybox", argv, envp, cwd);
}

static bool read_first_line(const char *path, char *buf, int buf_cap)
{
    if (buf_cap <= 0)
    {
        return false;
    }

    int fd = openat(AT_FDCWD, path, O_RDONLY);
    if (fd < 0)
    {
        return false;
    }

    int cursor = 0;
    while (cursor + 1 < buf_cap)
    {
        char ch = '\0';
        int cur = read(fd, &ch, 1);
        if (cur <= 0 || ch == '\n' || ch == '\r')
        {
            break;
        }
        buf[cursor++] = ch;
    }
    buf[cursor] = '\0';
    close(fd);
    return true;
}

static bool line_equals(const char *lhs, const char *rhs)
{
    int i = 0;
    while (lhs[i] != '\0' && rhs[i] != '\0')
    {
        if (lhs[i] != rhs[i])
        {
            return false;
        }
        ++i;
    }
    return lhs[i] == rhs[i];
}

static bool is_bash_shebang(const char *line)
{
    return line_equals(line, "#!/bin/bash") ||
           line_equals(line, "#! /bin/bash") ||
           line_equals(line, "#!/usr/bin/env bash") ||
           line_equals(line, "#! /usr/bin/env bash");
}

static void run_busybox_sed_inplace(char *envp[], const char *path, const char *script, const char *action)
{
    char *argv[] = {
        (char *)"busybox",
        (char *)"sed",
        (char *)"-i",
        (char *)script,
        (char *)path,
        0,
    };
    int ret = run_busybox(argv, envp);
    if (ret != 0)
    {
        printf("[shell] %s失败: %s ret=%d\n", action, path, ret);
    }
}

static void ensure_executable(char *envp[], const char *path)
{
    char *argv[] = {
        (char *)"busybox",
        (char *)"chmod",
        (char *)"755",
        (char *)path,
        0,
    };
    int ret = run_busybox(argv, envp);
    if (ret != 0)
    {
        printf("[shell] 补执行位失败: %s ret=%d\n", path, ret);
    }
}

static void normalize_script_entry(char *envp[], const char *path)
{
    char first_line[128];
    if (!read_first_line(path, first_line, sizeof(first_line)))
    {
        return;
    }

    if (first_line[0] != '#' || first_line[1] != '!')
    {
        run_busybox_sed_inplace(envp, path, "1i#!/bin/sh", "补 sh shebang");
    }
    else if (is_bash_shebang(first_line))
    {
        run_busybox_sed_inplace(envp, path,
                                "1s@^#! */bin/bash$@#!/bin/sh@;1s@^#! */usr/bin/env bash$@#!/bin/sh@",
                                "改写 bash shebang");
    }

    ensure_executable(envp, path);
}

static void normalize_shell_script_environment(char *envp[])
{
    // 只修正当前交互 shell 最常用、也最容易踩坑的几类脚本入口，避免影响评测模式。
    normalize_script_entry(envp, "/musl/libctest_testcode.sh");
    normalize_script_entry(envp, "/musl/libcbench_testcode.sh");
    normalize_script_entry(envp, "/musl/run-static.sh");
    normalize_script_entry(envp, "/musl/run-dynamic.sh");
    normalize_script_entry(envp, "/musl/unixbench_testcode.sh");
    normalize_script_entry(envp, "/musl/multi.sh");
    normalize_script_entry(envp, "/musl/tst.sh");
    normalize_script_entry(envp, "/glibc/libctest_testcode.sh");
    normalize_script_entry(envp, "/glibc/libcbench_testcode.sh");
    normalize_script_entry(envp, "/glibc/run-static.sh");
    normalize_script_entry(envp, "/glibc/run-dynamic.sh");
    normalize_script_entry(envp, "/glibc/unixbench_testcode.sh");
    normalize_script_entry(envp, "/glibc/multi.sh");
    normalize_script_entry(envp, "/glibc/tst.sh");
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

    normalize_shell_script_environment(envp);

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
        printf("#### F7LY INTERACTIVE SHELL END ret=%d ####\n", shell_ret);
        print_fuckyou();
        
        shutdown();
        return 0;
    }
}

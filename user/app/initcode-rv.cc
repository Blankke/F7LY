#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // basic_test("/mnt/musl/");
        // basic_test("/mnt/glibc/");
        // busybox_test("/mnt/musl/");
        // busybox_test("/mnt/glibc/");
        // libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        // lua_test("/mnt/musl/");
        // lua_test("/mnt/glibc/");

        // libcbench_test("/mnt/glibc/");
        // chdir("/mnt/musl/");
        // char *bb_sh[8] = {0};
        // bb_sh[0] = "busybox";
        // bb_sh[1] = "sh";
        // bb_sh[2] = "libcbench_testcode.sh";
        // execve("busybox", bb_sh, 0);
        // // lmbench_test("/mnt/musl/");

        // int fd = openat(-100, "/mnt/musl/basic_testcode.sh", 0);
        // char *buf[100];
        // read(fd, buf, 100);

        printf("initcode-rv: init_main start\n");
        // int fd = openat(-100, "/dev/null", 0);
        int fd = openat(-100, "/glibc/basic_testcode.sh", 0);
        printf("initcode-rv: openat /glibc/basic_testcode.sh fd=%d\n", fd);
        shutdown();
        return 0;
    }
}
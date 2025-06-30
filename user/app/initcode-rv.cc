#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup")))
    int main()
    {
        // basic_test("/mnt/musl/");
        // basic_test("/mnt/glibc/");
        // busybox_test("/mnt/musl/");
        // busybox_test("/mnt/glibc/");
        // libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        // lua_test("/mnt/musl/");
        // lua_test("/mnt/glibc/");
        // libcbench_test("/mnt/musl/");
        // lmbench_test("/mnt/musl/");
        iozone_test("/mnt/musl/");
        shutdown();
        return 0;
    }
}
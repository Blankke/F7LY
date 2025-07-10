#include "user.hh"

extern "C"
{
    int main()
    {
        libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        lua_test("/mnt/musl/");
        lua_test("/mnt/glibc/");
        basic_test("/mnt/musl/");
        basic_test("/mnt/glibc/");
        busybox_test("/mnt/musl/");
        busybox_test("/mnt/glibc/");

        // libcbench_test("/mnt/musl/");
        shutdown();
        return 0;
    }
}

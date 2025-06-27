#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup")))
    int main()
    {
        // start_shell();
        // basic_musl_test();
        // basic_glibc_test();   //  /lib/ld-linux-riscv64-lp64d.so.1
        // busybox_musl_test();
        // busybox_glibc_test();
        // libc_musl_test();   // /lib/ld-musl-riscv64-sf.so.1
        // lmbench_test();
        libcbench_test();
        // iozone_test();
        // lmbench_test();
        // basic_test("/mnt/musl/");
        // basic_test("/mnt/glibc/");
        // busybox_test("/mnt/musl/");
        // busybox_test("/mnt/glibc/");
        // libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        // lua_test("/mnt/musl/");
        // lua_test("/mnt/glibc/");
        shutdown();
        return 0;
    }
}
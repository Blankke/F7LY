#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // basic_test("musl/");
        // basic_test("glibc/");
        // busybox_test("/musl/");
        // busybox_test("glibc/");
        // libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        // lua_test("/mnt/musl/");
        // lua_test("/mnt/glibc/");

        // libcbench_test("/glibc");
        // ltp_test("/musl/ltp/testcases/bin/");


        // 决赛测例
        // run_test("/musl/interrupts-test-1");
        // run_test("/musl/interrupts-test-2");
        shutdown();
        return 0;
    }
}
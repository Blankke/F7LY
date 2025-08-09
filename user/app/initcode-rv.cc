#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
        // basic_test("musl/");
        // basic_test("glibc/");
        // busybox_test("/musl/");
        // busybox_test("glibc/");
        // libc_test("/mnt/musl/"); // 不测glibc, 不要求测
        // lua_test("/mnt/musl/");
        // lua_test("/mnt/glibc/");

        // libcbench_test("/glibc");
        ltp_test("/musl/ltp/testcases/bin/");

        // 决赛测例
        // final_test_musl();
        // final_test_glibc();
        // 现场赛测例
        //  git_test("/musl/usr/bin");
        shutdown();
        return 0;
    }
}
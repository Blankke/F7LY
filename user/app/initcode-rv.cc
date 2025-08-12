#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/musl/");
        // ltp_test(true);
        ltp_test(false);
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // busybox_test("/musl/");
        // busybox_test("/glibc/");
        // libc_test("/musl/"); // 不测glibc, 不要求测
        // // lua_test("/musl/");//////////吗，rsju
        // lua_test("/glibc/");
        // libcbench_test("/glibc");

        // 决赛测例
        // final_test_musl();
        // final_test_glibc();
        // 现场赛测例
        //  git_test("/musl/usr/bin");
        shutdown();
        return 0;
    }
}
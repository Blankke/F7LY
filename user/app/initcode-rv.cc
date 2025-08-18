#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/musl/");
        // iozone_test("/musl");
        // iozone_test("/glibc");
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // ltp_test(true);
        // ltp_test(false);
        // busybox_test("/musl/");
        // busybox_test("/glibc/");
        // libc_test("/musl/"); // 不测glibc, 不要求测
        // lua_test("/musl/");//////////吗，rsju
        // lua_test("/glibc/");
        // libcbench_test("/musl");
        // libcbench_test("/glibc");
        // lmbench_test("/glibc");
        // lmbench_test("/musl");
        // 决赛测例
        // final_test_musl();
        // final_test_glibc();
        // 现场赛测例
        git_test("/musl");
        shutdown();
        return 0;
    }
}
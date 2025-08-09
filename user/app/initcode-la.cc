#include "user.hh"

extern "C"
{
    int main()
    {
        init_env("/musl/");
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // busybox_test("/musl/");
        // busybox_test("/glibc/");

        // libcbench_test("/glibc");
        // ltp_test("/musl/ltp/testcases/bin/");

        // 决赛测例
        sleep(5);
        final_test_musl();
        final_test_glibc();

        shutdown();
        return 0;
    }
}

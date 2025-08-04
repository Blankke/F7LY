#include "user.hh"

extern "C"
{
    int main()
    {
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // busybox_test("/musl/");
        // busybox_test("/glibc/");

        // libcbench_test("/glibc");
        ltp_test("/musl/ltp/testcases/bin/");

        // 决赛测例
        // run_test("/musl/interrupts-test-1");
        // run_test("/musl/interrupts-test-2");
        // run_test("/musl/copy-file-range-test-1");
        // run_test("/musl/copy-file-range-test-2");
        // run_test("/musl/copy-file-range-test-3");
        // run_test("/musl/copy-file-range-test-4");
        // run_test("/musl/test_splice");

        shutdown();
        return 0;
    }
}

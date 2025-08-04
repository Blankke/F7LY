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
        ltp_test("/musl/ltp/testcases/bin/");

        // 决赛测例
        // run_test("/musl/interrupts-test-1");
        // run_test("/musl/interrupts-test-2");
        // run_test("/musl/copy-file-range-test-1");
        // run_test("/musl/copy-file-range-test-2");
        // run_test("/musl/copy-file-range-test-3");
        // run_test("/musl/copy-file-range-test-4");
        // char *splice_argv[] = {"test_splice","1", NULL};
        // run_test("/musl/test_splice", splice_argv, 0);
        // char *splice_argv[] = {"test_splice","2", NULL};
        // run_test("/musl/test_splice", splice_argv, 0);
        // char *splice_argv[] = {"test_splice","3", NULL};
        // run_test("/musl/test_splice", splice_argv, 0);
        // char *splice_argv[] = {"test_splice","4", NULL};
        // run_test("/musl/test_splice", splice_argv, 0);
        // char *splice_argv[] = {"test_splice","5", NULL};
        // run_test("/musl/test_splice", splice_argv, 0);


        //现场赛测例
        // git_test("/musl/usr/bin");
        shutdown();
        return 0;
    }
}
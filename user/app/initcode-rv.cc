#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // 临时只跑 RISC-V musl 的 libcbench，用 syscall/page fault trace 缩小崩溃路径。
        init_env("/musl/");
        // netperf_test("/musl/");
        // netperf_test("/glibc/");
        // iperf_test("/musl/");
        // iperf_test("/glibc/");
        // iozone_test("/musl");
        // iozone_test("/glibc");
        // libc_test("/musl/");
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // lua_test("/musl/");
        // lua_test("/glibc/");
        // libcbench_test("/musl");
        // libcbench_test("/glibc");
        // ltp_test(true);
        // ltp_test(false);
        // iozone_priority_borrow_research();
        // busybox_test("/musl/");
        // busybox_test("/glibc/");
        libcbench_test("/musl");
        shutdown();
        return 0;
    }
}

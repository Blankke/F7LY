#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
        // 先把 LTP 前置，便于性能回归时优先观察 fs_bind/ltp 脚本段推进速度。
        ltp_test(false);
        ltp_test(true);
        libc_test("/musl/");
        basic_test("/musl/");
        basic_test("/glibc/");
        lua_test("/musl/");
        lua_test("/glibc/");
        netperf_test("/musl/");
        netperf_test("/glibc/");
        iperf_test("/musl/");
        iperf_test("/glibc/");
        busybox_test("/musl/");
        busybox_test("/glibc/");
        libcbench_test("/musl");
        libcbench_test("/glibc");
        iozone_test("/glibc");
        iozone_test("/musl");
        lmbench_test("/musl/");
        lmbench_test("/glibc/");
        shutdown();
        return 0;
    }
}

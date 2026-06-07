#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
        ltp_test(true);
        ltp_test(false);
        basic_test("/musl/");
        basic_test("/glibc/");
        lua_test("/musl/");
        lua_test("/glibc/");
        libc_test("/musl/");
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

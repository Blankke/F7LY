#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
        libc_test("/musl/");
        ltp_test(false);
        ltp_test(true);
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

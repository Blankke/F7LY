#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
        // 当前先把 LoongArch 默认入口收敛到 libc 与后续常规套件，避免 LTP 长套件
        // 掩盖 libctest/VMA 回归；需要跑 LTP 时再显式恢复这两行。
        // ltp_test(false);
        // ltp_test(true);
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

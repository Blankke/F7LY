#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        init_env("/musl/");
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
        // busybox_test("/musl/");
        // busybox_test("/glibc/");
        shutdown();
        return 0;
    }
}

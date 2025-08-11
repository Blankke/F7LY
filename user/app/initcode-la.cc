#include "user.hh"

extern "C"
{
    int main()
    {
        init_env("/musl/");
        ltp_test(true);
        // ltp_test(false);
        // basic_test("/musl/");
        // basic_test("/glibc/");
        // busybox_test("/musl/");
        // busybox_test("/glibc/");
        // libc_test("/musl/"); // 不测glibc, 不要求测
        // lua_test("/musl/");
        // lua_test("/glibc/");

        shutdown();
        return 0;
    }
}

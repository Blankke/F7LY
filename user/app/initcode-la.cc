#include "user.hh"

extern "C"
{
    int main()
    {
        init_env("/musl/");
        lua_test("/musl/");
        lua_test("/glibc/");
        basic_test("/musl/");
        basic_test("/glibc/");
        ltp_test(true);
        // ltp_test(false);
        busybox_test("/musl/");
        busybox_test("/glibc/");
        libc_test("/musl/"); // 不测glibc, 不要求测

        shutdown();
        return 0;
    }
}

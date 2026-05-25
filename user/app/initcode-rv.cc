#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        // 本轮只调试 LTP，避免 basic/busybox/libc 等非目标套件干扰长跑日志。
        init_env("/musl/");
        ltp_test(true);
        ltp_test(false);
        shutdown();
        return 0;
    }
}

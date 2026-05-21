#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        ltp_test(true);     
        // regression_suite_4d1444_riscv();
        shutdown();
        return 0;
    }
}

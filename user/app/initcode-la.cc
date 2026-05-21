#include "user.hh"


extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        regression_suite_4d1444_loongarch();
        shutdown();
        return 0;
    }
}

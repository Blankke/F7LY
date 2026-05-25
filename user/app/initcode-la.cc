#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        regression_suite_4d1444();
        // iozone_test("/musl");
        shutdown();
        return 0;
    }
}

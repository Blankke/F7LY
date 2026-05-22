#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        basic_musl_test();
        shutdown();
        return 0;
    }
}

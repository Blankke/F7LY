#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        regression_suite_4d1444();
        shutdown();
        return 0;
    }
}

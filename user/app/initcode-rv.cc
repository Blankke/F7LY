#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4();
        regression_rank_probe();
        shutdown();
        return 0;
    }
}

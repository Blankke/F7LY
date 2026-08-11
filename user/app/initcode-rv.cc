#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        cagent_test();
        buildstorm_test();
        shutdown();
        return 0;
    }
}

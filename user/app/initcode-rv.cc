#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup")))
    int main()
    {
        basic_test();
        shutdown();
        return 0;
    }
}
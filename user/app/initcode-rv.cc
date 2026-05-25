#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        iozone_test("/musl/");
        shutdown();
        return 0;
    }
}

#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        chdir("/fat32");
        basic_glibc_test();
        shutdown();
        return 0;
    }
}
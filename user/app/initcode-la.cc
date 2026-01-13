#include "user.hh"

extern "C"
{
    int main()
    {
        chdir("/fat32");
        basic_glibc_test();
        shutdown();
        return 0;
    }
}

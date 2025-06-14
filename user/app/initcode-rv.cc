#include "user.hh"

extern "C"
{
    int main()
    {
        // basic_test();
        busybox_musl_test();
        // start_shell();
        shutdown();
        return 0;
    }
}
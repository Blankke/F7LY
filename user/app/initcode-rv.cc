#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup")))
    int main()
    {
        // basic_test();
        // busybox_musl_test();
        // start_shell();
        libc_musl_test();
        // libcbench_test();
        // iozone_test();
        shutdown();
        return 0;
    }
}
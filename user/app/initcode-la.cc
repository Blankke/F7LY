#include "user.hh"

extern "C"
{
    int main()
    {
        // basic_test();
        // busybox_musl_test();
        // start_shell();
        // libc_musl_test();
        // libcbench_test();
        // iozone_test();
        // basic_test("/mnt/musl/");
        // basic_test("/mnt/glibc/");
        busybox_test("/mnt/musl/");

        shutdown();
        return 0;
    }
}

#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        userdebug4();
        static const char *const debug_ltp_cases[] = {"memfd_create01", 0};
        init_env("/musl/");
        ltp_subset_test(true, debug_ltp_cases);
        shutdown();
        return 0;
    }
}

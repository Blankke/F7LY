#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
    static const char *const debug_ltp_cases[] = {"getpid01", 0};
    userdebug3();
    init_env("/musl/");
    ltp_subset_test(true, debug_ltp_cases);
        shutdown();
        return 0;
    }
}

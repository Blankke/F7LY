#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        userdebug3();
        static const char *const ltp_cases[] = {
            "access02",
            NULL,
        };
        init_env("/musl/");
        ltp_subset_test(false, ltp_cases);
        shutdown();
        return 0;
    }
}

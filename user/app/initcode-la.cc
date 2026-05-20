#include "user.hh"


extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        static const char *const debug_basic_cases[] = {"write", 0};
        userdebug3();
        init_env("/musl/");
        basic_subset_test("/musl/", debug_basic_cases);
        shutdown();
        return 0;
    }
}

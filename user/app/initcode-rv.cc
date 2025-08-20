#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/");
        // 现场赛测例
        git_test("/proj");
        shutdown();
        return 0;
    }
}
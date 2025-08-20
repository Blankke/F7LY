#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/musl/");
        // 现场赛测例
        // git_test("/musl");
        vim_h();
        shutdown();
        return 0;
    }
}
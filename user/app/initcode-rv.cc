#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/");
        // 现场赛测例
        // git_test("/musl");
        vim_h();
        gcc_test();
      // rustc_test();
        // 运行交互式shell

        shutdown();
        return 0;
    }
}
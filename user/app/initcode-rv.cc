#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/");
        // 现场赛测例
        // git_test("/musl");
        gcc_test();
        vim_h();
      // run_test("/usr/bin/vim", bb_sh, 0);
        // 运行交互式shell

        shutdown();
        return 0;
    }
}
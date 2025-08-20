#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // init_env("/");
        // 现场赛测例
        // git_test("/musl");
        vim_h();
        // chdir("/usr");
      // char *bb_sh[2] = {0};
      // bb_sh[0] = "/usr/bin/vim";
      // bb_sh[1] = "hello.c";
      // run_test("/usr/bin/vim", bb_sh, 0);
        // 运行交互式shell

        shutdown();
        return 0;
    }
}
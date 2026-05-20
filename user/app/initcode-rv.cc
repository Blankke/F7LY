#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        userdebug3();
        init_env("/glibc/");
        char *du_args[] = {(char *)"busybox", (char *)"du", 0};
        run_test("busybox", du_args, 0);
        shutdown();
        return 0;
    }
}

#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // 2026 决赛两题均只使用 glibc，镜像内入口统一位于 /glibc。
        cagent_test();
        buildstorm_test();
        shutdown();
        return 0;
    }
}

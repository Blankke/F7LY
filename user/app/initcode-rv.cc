#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        // userdebug4(); 
        iozone_mclock_research_riscv();
        shutdown();
        return 0;
    }
}

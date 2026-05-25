#include "user.hh"

extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        iozone_mclock_research();
        shutdown();
        return 0;
    }
}

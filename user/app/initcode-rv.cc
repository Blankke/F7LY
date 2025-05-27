#include "user.hh"
extern "C"
{
    int main()
    {
        openat(1, "ddd", 3);
        // while(1);
        while(1);
        shutdown();
        return 0;
    }
}
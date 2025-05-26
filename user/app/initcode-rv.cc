#include "user.hh"
extern "C"
{
    int main()
    {
        int a = 5;
        while(a--);
        openat(1, "ddd", 3);
        shutdown();
        return 0;
    }
}
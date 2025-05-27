#include "user.hh"
extern "C"
{
    int main()
    {
        openat(1, "ddd", 3);
    }
}
#include "user.hh"
extern "C"
{
    int main()
    {
        printf("Hello, World!\n");
        // while(1);
        int a = 7;
        printf("a = %d\n", a);
        while(a--);
        printf("a = %d\n", a);
        shutdown();
        return 0;
    }
}
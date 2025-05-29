#include "user.hh"
#include "fuckyou.hh"
extern "C"
{
    int main()
    {
        printf("Hello, World!\n");
        print_fuckyou();
        while(1);
        int a = 7;
        printf("a = %d\n", a);
        while(a--);
        printf("a = %d\n", a);
        shutdown();
        return 0;
    }
}
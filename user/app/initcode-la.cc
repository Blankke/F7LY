#include "user.hh"

extern "C"
{
    int main()
    {
        git_test("/proj");
        shutdown();
        return 0;
    }
}

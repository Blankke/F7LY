#include "trap_func_wrapper.hh"
#include "trap.hh"

extern "C"{
    void wrap_kerneltrap()
    {
        trap_mgr.kerneltrap();
    }

    //!!写完进程后修改
    void wrap_usertrap()
    {
        trap_mgr.usertrap();
    }
    
    void wrap_usertrapret()
    {
        trap_mgr.usertrapret();
    }
}
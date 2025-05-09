#include "trap_func_wrapper.hh"
#include "trap.hh"
#include "sbi.hh"
extern "C"{
    void kerneltrap()
    {
        sbi_console_putchar('a');
        trap_mgr.kerneltrap();
    }

    //!!写完进程后修改
    // void usertrap()
    // {
    //     trap_mgr.usertrap();
    // }
    
    // void usertrapret()
    // {
    //     trap_mgr.trapret();
    // }
}
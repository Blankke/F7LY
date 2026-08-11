#ifdef LOONGARCH
#include "trap_func_wrapper.hh"
#include "trap.hh"

extern "C"{
    void wrap_kerneltrap(const uint64 *saved_frame)
    {
        trap_mgr.kerneltrap(saved_frame);
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
    void wrap_machine_trap()
    {
        trap_mgr.machine_trap();
    }
}
#endif

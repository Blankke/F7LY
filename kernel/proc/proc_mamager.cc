#include"proc_mamager.hh"


namespace proc
{
    ProcessManager k_pm;

    void ProcessManager::init(const char *pid_lock_name, const char *wait_lock_name)
    {
        _pid_lock.init(pid_lock_name);
        _wait_lock.init(wait_lock_name);
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb &p = k_proc_pool[i];
            p.init("pcb", i);
        }
        _cur_pid = 1;
        _last_alloc_proc_gid = num_process - 1;
    }

    Pcb *ProcessManager::get_cur_pcb()
    {
        loongarch::Cpu::push_intr_off();
        loongarch::Cpu *c_cpu = loongarch::Cpu::get_cpu();
        proc::Pcb *pcb = c_cpu->get_cur_proc();
        loongarch::Cpu::pop_intr_off();
        return pcb;
    }
}
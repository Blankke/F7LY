#include"proc_manager.hh"
#include "hal/cpu.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "libs/klib.hh"


extern "C"
{

    extern int init_main(void);

    void _wrp_fork_ret(void)
    {
        // proc::k_pm.fork_ret();
    }
}

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
        Cpu::push_intr_off();
        Cpu *c_cpu = Cpu::get_cpu();
        proc::Pcb *pcb = c_cpu->get_cur_proc();
        Cpu::pop_intr_off();
        return pcb;
    }

    void ProcessManager::alloc_pid(Pcb *p)
    {
        _pid_lock.acquire();
        p->_pid = _cur_pid;
        _cur_pid++;
        _pid_lock.release();
    }

    Pcb *ProcessManager::alloc_proc()
    {
        Pcb *p;
        for (uint i = 0; i < num_process; i++)
        {
            p = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            p->_lock.acquire();
            if (p->_state == ProcState::UNUSED)
            {
                k_pm.alloc_pid(p);
                p->_state = ProcState::USED;
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                // p->_shm = mem::vml::vm_trap_frame - 64 * 2 * mem::PageEnum::pg_size;
                // p->_shmkeymask = 0;
                // k_pm.set_vma( p );

                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

                _proc_create_vm(p);
                if (p->_pt.get_base() == 0)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

                p->_mqmask = 0;

                memset(&p->_context, 0, sizeof(p->_context));

                p->_context.ra = (uint64)_wrp_fork_ret;

                p->_context.sp = p->_kstack + PGSIZE;

                p->_lock.release();

                _last_alloc_proc_gid = p->_gid;

                return p;
            }
            else
            {
                p->_lock.release();
            }
        }
        return nullptr;
    }

    void ProcessManager::_proc_create_vm(Pcb *p)
    {
        //TODO
    }

    void ProcessManager::freeproc(Pcb *p)
    {
        //TODO
    }
    void ProcessManager::exit(int state)
    {
        //TODO
    }
    // int ProcessManager::wait(int child_pid, uint64 addr)
    // {
    //     //TODO
    // }
    int ProcessManager::get_cur_cpuid()
    {
        return r_tp();
    }
    
    
    void ProcessManager::user_init(){
        Pcb *p = alloc_proc();
        assert( p != nullptr, "pm: alloc proc fail while user init." );
        _init_proc = p;
        p->_lock.acquire();
        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        



    }
}

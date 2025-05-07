#include "proc.hh"
#include "context.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
namespace proc
{
    Pcb k_proc_pool[num_process]; // 全局进程池

    Pcb::Pcb()
    {
        _state = UNUSED;
        _chan = nullptr;
        _killed = 0;
        _xstate = 0;
        _pid = 0;
        parent = nullptr;
        _sz = 0;
        _trapframe = nullptr;
        _kstack = 0;
        _slot = 0;
        _priority = default_proc_prio;
    }

    void Pcb::init(const char *lock_name, uint gid)
    {
        _lock.init(lock_name);
        _state = ProcState::UNUSED;
        _gid = gid;
        _kstack = mem::VirtualMemoryManager::kstack_vm_from_gid(_gid);
        //TODO
        // for (auto &of : _ofile)
        //     of = nullptr;
    }

    void Pcb::map_kstack(mem::PageTable &pt)
    {
#ifdef RISCV
        if (_kstack == 0)
            panic("pcb was not init");

        char *pa = (char *)mem::k_pmm.alloc_page();
        if (pa == 0)
            panic("pcb map kstack: no memory");
        mem::k_pmm.clear_page((void *)pa);
        if (!mem::k_vmm.map_pages(pt, _kstack, PGSIZE, (uint64)pa,
                                  riscv::PteEnum::pte_readable_m |
                                      riscv::PteEnum::pte_writable_m))
            panic("kernel vm map failed");
#elif defined(LOONGARCH)


#endif
    }
}
    
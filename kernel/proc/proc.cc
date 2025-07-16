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
        _parent = nullptr;
        _sz = 0;
        _shared_vm = false;
#ifdef LOONGARCH
        elf_base = 0; // 初始化 ELF 基地址为 0
#endif
        _trapframe = nullptr;
        _kstack = 0;
        _slot = 0;
        _priority = default_proc_prio;
        _ofile = nullptr;  // 初始化为nullptr，在init中分配
        
        // 初始化新增字段
        _ppid = 0;
        _pgid = 0;
        _sid = 0;
        _uid = 0;
        _euid = 0;
        _gid = 0;
        _egid = 0;
        _start_tick = 0;      // 初始化进程开始时间
        _user_ticks = 0;      // 初始化用户态时间
        _last_user_tick = 0;  // 初始化上次用户态时间
        _kernel_entry_tick = 0; // 初始化内核态进入时间
        _stime = 0;
        _cutime = 0;
        _cstime = 0;
        _start_time = 0;
        _start_boottime = 0;
    }

    void Pcb::init(const char *lock_name, uint gid)
    {
        _lock.init(lock_name);
        _state = ProcState::UNUSED;
        _global_id = gid;
        _kstack = mem::VirtualMemoryManager::kstack_vm_from_global_id(_global_id);
               
        // TODO: 资源限制
        _rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_cur = 0;
        _rlim_vec[ResourceLimitId::RLIMIT_STACK].rlim_max = 0;
        
        // 设置打开文件数量限制
        _rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_cur = max_open_files;
        _rlim_vec[ResourceLimitId::RLIMIT_NOFILE].rlim_max = max_open_files;
        _sigmask = 0;
        _signal = 0;
    }

    void Pcb::cleanup_sighand()
    {
        if (_sigactions != nullptr)
        {
            _sigactions->refcnt--;
            if (_sigactions->refcnt <= 0)
            {
                // 引用计数为0，释放所有信号处理函数
                for (int i = 0; i <= ipc::signal::SIGRTMAX; ++i)
                {
                    if (_sigactions->actions[i] != nullptr)
                    {
                        delete _sigactions->actions[i];
                        _sigactions->actions[i] = nullptr;
                    }
                }
                delete _sigactions;
            }
            _sigactions = nullptr;
        }
    }

    void Pcb::cleanup_ofile()
    {
        if (_ofile != nullptr)
        {
            _ofile->_shared_ref_cnt--;
            if (_ofile->_shared_ref_cnt <= 0)
            {
                // 引用计数为0，释放所有打开的文件
                for (uint64 i = 0; i < max_open_files; ++i)
                {
                    if (_ofile->_ofile_ptr[i] != nullptr)
                    {
                        _ofile->_ofile_ptr[i]->free_file();
                        _ofile->_ofile_ptr[i] = nullptr;
                    }
                }
                delete _ofile;
            }
            _ofile = nullptr;
        }
    }

    void Pcb::map_kstack(mem::PageTable &pt)
    {

        if (_kstack == 0)
            panic("pcb was not init");

        char *pa = (char *)mem::k_pmm.alloc_page();
        if (pa == 0)
            panic("pcb map kstack: no memory");
        mem::k_pmm.clear_page((void *)pa);
#ifdef RISCV
        // printfBlue("map kstack: %p, end: %p\n", _kstack, _kstack + PGSIZE-1);
        if (!mem::k_vmm.map_pages(pt, _kstack, PGSIZE, (uint64)pa,
                                  riscv::PteEnum::pte_readable_m |
                                      riscv::PteEnum::pte_writable_m))
            panic("kernel vm map failed");
#elif defined(LOONGARCH)
        /// TODO:未测试正确性
        /// copy from 华科
        if (!mem::k_vmm.map_pages(pt, _kstack, PGSIZE, (uint64)pa,
                                  PTE_NX | PTE_P | PTE_W | PTE_MAT | PTE_D | PTE_PLV))
            panic("kernel vm map failed");
#endif
    }

    int Pcb::get_priority()
    {
        _lock.acquire();
        int priority = _priority;
        _lock.release();
        return priority;
    }

    void Pcb::print_context()
    {
        printf("Context register values for process %d (%s):\n", this->_pid, this->_name);
        print_context1(this->get_context());
    }

    void print_context1(Context *ctx)
    {
        printf(" ra  = 0x%x\n", ctx->ra);
        printf(" sp  = 0x%x\n", ctx->sp);
        printf(" s0  = 0x%x\n", ctx->s0);
        printf(" s1  = 0x%x\n", ctx->s1);
        printf(" s2  = 0x%x\n", ctx->s2);
        printf(" s3  = 0x%x\n", ctx->s3);
        printf(" s4  = 0x%x\n", ctx->s4);
        printf(" s5  = 0x%x\n", ctx->s5);
        printf(" s6  = 0x%x\n", ctx->s6);
        printf(" s7  = 0x%x\n", ctx->s7);
        printf(" s8  = 0x%x\n", ctx->s8);
#ifdef RISCV
        printf(" s9  = 0x%x\n", ctx->s9);
        printf(" s10 = 0x%x\n", ctx->s10);
        printf(" s11 = 0x%x\n", ctx->s11);
#endif
    }
}

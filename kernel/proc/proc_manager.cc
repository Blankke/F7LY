#include "proc_manager.hh"
#include "hal/cpu.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "scheduler.hh"
#include "libs/klib.hh"
#include "trap.hh"
#include "printer.hh"

extern "C"
{
    extern uint64 initcode_start[];
    extern uint64 initcode_end[];

    extern int init_main(void);
    extern char trampoline[]; // trampoline.S
    void _wrp_fork_ret(void)
    {
        // printf("into _wrapped_fork_ret\n");
        proc::k_pm.fork_ret();
    }
}

namespace proc
{
    ProcessManager k_pm;

    void ProcessManager::init(const char *pid_lock_name, const char *wait_lock_name)
    {
        // initialize the proc table.
        _pid_lock.init(pid_lock_name);
        _wait_lock.init(wait_lock_name);
        for (uint i = 0; i < num_process; ++i)
        {
            Pcb &p = k_proc_pool[i];
            p.init("pcb", i);
        }
        _cur_pid = 1;
        _last_alloc_proc_gid = num_process - 1;
        printfGreen("[pm] Process Manager Init\n");
    }

    Pcb *ProcessManager::get_cur_pcb()
    {
        Cpu::push_intr_off();
        Cpu *c_cpu = Cpu::get_cpu();
        proc::Pcb *pcb = c_cpu->get_cur_proc();
        Cpu::pop_intr_off();
        // 这里为nullptr是正常现象应该无需panic？
        // 学长未对此处作处理，而是判断为nullptr就sleep，参考virtio_disk.cc:218行
        // commented out by @gkq
        //
        // if (pcb == nullptr) 
        //     panic("get_cur_pcb: no current process");
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
        // 遍历整个进程池，尝试分配一个 UNUSED 的进程控制块
        for (uint i = 0; i < num_process; i++)
        {
            // 使用轮转式分配策略，避免总是从头找，提高公平性
            p = &k_proc_pool[(_last_alloc_proc_gid + i) % num_process];
            p->_lock.acquire();
            if (p->_state == ProcState::UNUSED)
            {
                k_pm.alloc_pid(p);
                p->_state = ProcState::USED;
                // 设置调度相关字段：默认调度槽与优先级
                p->_slot = default_proc_slot;
                p->_priority = default_proc_prio;

                // p->_shm = mem::vml::vm_trap_frame - 64 * 2 * mem::PageEnum::pg_size;
                // p->_shmkeymask = 0;
                // k_pm.set_vma( p );

                // 为该进程分配一页 trapframe 空间（用于中断时保存用户上下文）
                if ((p->_trapframe = (TrapFrame *)mem::k_pmm.alloc_page()) == nullptr)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }

                // 创建进程自己的页表（空的页表）


                //debug
                printfCyan("[user pgtbl]==>into proc_pagetable\n");
                _proc_create_vm(p);


                if (p->_pt.get_base() == 0)
                {
                    freeproc(p);
                    p->_lock.release();
                    return nullptr;
                }
                // 清空消息队列掩码
                p->_mqmask = 0;

                // 初始化上下文结构体
                memset(&p->_context, 0, sizeof(p->_context));

                // 设置调度返回地址为 _wrp_fork_ret
                // 当调度器切换回该进程时，将从这里开始执行
                p->_context.ra = (uint64)_wrp_fork_ret;
                printf("p->_context.ra: %p\n", p->_context.ra);
                printf("wrp_fork_ret: %p\n", _wrp_fork_ret);
                void (ProcessManager::*fptr)() = &ProcessManager::fork_ret;
                printf("ProcessManager::fork_ret: %p\n", (void *)*(uintptr_t *)&fptr);

                // 设置内核栈指针
                p->_context.sp = p->_kstack + PGSIZE;

                p->_lock.release();

                // 更新上次分配的位置，轮转分配策略
                _last_alloc_proc_gid = p->_gid;

                return p;
            }
            else
            {
                p->_lock.release();
            }
        }
        // 没有找到可用的进程控制块，分配失败
        return nullptr;
    }

    void ProcessManager::fork_ret()
    {
        static int first = 1;
        get_cur_pcb()->_lock.release();

        if (first)
        {
            first = 0;
            TODO(
                // printf("sp: %x\n", r_sp());
                filesystem_init();
                filesystem2_init(); // 启动init
            )
        }
        // printf("fork_ret\n");
        trap_mgr.usertrapret();
    }

    void ProcessManager::_proc_create_vm(Pcb *p)
    {
        p->_pt = proc_pagetable(p);
    }

    void ProcessManager::freeproc(Pcb *p)
    {
        if (p->_trapframe)
            mem::k_pmm.free_page(p->_trapframe);
        p->_trapframe = 0;
        if (p->_pt.get_base())
            proc_freepagetable(p->_pt, p->_sz);
        p->_pt = 0;
        p->_sz = 0;
        p->_pid = 0;
        p->_parent = 0;
        p->_name[0] = 0;
        p->_chan = 0;
        p->_killed = 0;
        p->_xstate = 0;
        p->_state = ProcState::UNUSED;
    }
    void ProcessManager::exit(int state)
    {
        // TODO
    }
    // int ProcessManager::wait(int child_pid, uint64 addr)
    // {
    //     //TODO
    // }
    int ProcessManager::get_cur_cpuid()
    {
        return r_tp();
    }

    mem::PageTable ProcessManager::proc_pagetable(Pcb *p)
    {
        mem::PageTable pt = mem::k_vmm.vm_create();
        if (pt.is_null())
            printfRed("proc_pagetable: vm_create failed\n");
        if (pt.get_base() == 0)
            return 0;
#ifdef RISCV
        if (mem::k_vmm.map_pages(pt, TRAMPOLINE, PGSIZE, (uint64)trampoline, riscv::PteEnum::pte_readable_m|riscv::pte_executable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trampoline failed\n");
            return 0;
        }
        printfGreen("trampoline: %p\n", trampoline);
        printfGreen("TRAMPOLINE: %p\n", TRAMPOLINE);
        if (mem::k_vmm.map_pages(pt, TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()), riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_writable_m) == 0)
        {
            mem::k_vmm.vmfree(pt, 0);
            printfRed("proc_pagetable: map trapframe failed\n");
            return 0;
        }

#elif defined(LOONGARCH)
// TODO
#endif
        return pt;
    }
    void ProcessManager::proc_freepagetable(mem::PageTable pt, uint64 sz)
    {
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
        mem::k_vmm.vmfree(pt, 0);
#elif defined(LOONGARCH)
// TODO
#endif
    }

    void ProcessManager::user_init()
    {
#ifdef RISCV
        Pcb *p = alloc_proc();
        _init_proc = p;
        // 传入initcode的地址
        mem::k_vmm.uvmfirst(p->_pt, (uint64)initcode_start, (uint64)initcode_end - (uint64)initcode_start);
        //debug
        // uint64 pa = (uint64)p->_pt.walk_addr((uint64)0);
        // printfYellow("initcode start pa: %p\n",pa);
        // printfYellow("initcode start byte %u\n", *(uint64 *)pa);
        printf("initcode start: %p, end: %p\n", initcode_start, initcode_end);
        printf("initcode size: %p\n", (uint64)(initcode_end - 0));
        p->_sz = 3 * PGSIZE;

        p->_trapframe->epc = 0;
        p->_trapframe->sp = p->_sz;

        safestrcpy(p->_name, "initcode", sizeof(p->_name));
        p->_parent = p;
        safestrcpy(p->_cwd_name, "/", sizeof(p->_cwd_name));

        p->_state = ProcState::RUNNABLE;

        p->_lock.release();

#elif defined(LOONGARCH)
// TODO
#endif
    }

    // Atomically release lock and sleep on chan.
    // Reacquires lock when awakened.
    void ProcessManager::sleep(void *chan, SpinLock *lock)
    {
        Pcb *p = get_cur_pcb();
        // Must acquire p->lock in order to
        // change p->state and then call sched.
        // Once we hold p->lock, we can be
        // guaranteed that we won't miss any wakeup
        // (wakeup locks p->lock),
        // so it's okay to release lk.
        p->_lock.acquire();
        lock->release();

        // go to sleep
        p->_chan = chan;
        p->_state = ProcState::SLEEPING;
        k_scheduler.call_sched();

        p->_chan = 0;

        p->_lock.release();
        lock->acquire();
    }
    void ProcessManager::wakeup(void *chan)
    {
        Pcb *p;

        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p != k_pm.get_cur_pcb())
            {
                p->_lock.acquire();
                if (p->_state == ProcState::SLEEPING && p->_chan == chan)
                {
                    p->_state = ProcState::RUNNABLE;
                }
                p->_lock.release();
            }
        }
    }
    void ProcessManager::set_killed(Pcb *p)
    {
        p->_lock.acquire();
        p->_killed = 1;
        p->_lock.release();
    }
    // Kill the process with the given pid.
    // The victim won't exit until it tries to return
    // to user space (see usertrap() in trap.c).
    int ProcessManager::kill_proc(int pid)
    {
        Pcb *p;
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            p->_lock.acquire();

            // 如果找到目标 pid 的进程
            if (p->_pid == pid)
            {
                // 设置该进程的 killed 标志位为 1，
                // 表示该进程已被请求终止。
                // 被 kill 并不立即终止进程，而是在合适的时机由进程自行处理。
                p->_killed = 1;

                // 若该进程当前在 sleep（通常是等待 I/O 或锁）
                // 将其唤醒（设为 RUNNABLE），这样调度器会调度它运行，
                // 让它可以检查 _killed 并自行退出。
                if (p->_state == ProcState::SLEEPING)
                {
                    // 提前唤醒等待中的进程，
                    // 避免它永远睡着不被调度，也就永远无法响应 kill。
                    p->_state = ProcState::RUNNABLE;
                }

                p->_lock.release();
                return 0;
            }

            p->_lock.release();
        }
        return -1; // 没找到对应 pid 的进程
    }

    // Copy from either a user address, or kernel address,
    // depending on usr_src.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_in(void *dst, int user_src, uint64 src, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_src)
        {
            return mem::k_vmm.copy_in(p->_pt, dst, src, len);
        }
        else
        {
            memmove(dst, (char *)src, len);
            return len;
        }
    }
    // Copy to either a user address, or kernel address,
    // depending on usr_dst.
    // Returns 0 on success, -1 on error.
    int ProcessManager::either_copy_out(void *src, int user_dst, uint64 dst, uint64 len)
    {
        Pcb *p = get_cur_pcb();
        if (user_dst)
        {
            return mem::k_vmm.copy_out(p->_pt, dst, src, len);
        }
        else
        {
            memmove((char *)dst, src, len);
            return len;
        }
    }
    // Print a process listing to console.  For debugging.
    // Runs when user types ^P on console.
    // No lock to avoid wedging a stuck machine further.
    void ProcessManager::procdump()
    {
        static const char *states[6] = {
            "unused", // ProcState::UNUSED
            "used",   // ProcState::USED
            "sleep ", // ProcState::SLEEPING
            "runble", // ProcState::RUNNABLE
            "run   ", // ProcState::RUNNING
            "zombie"  // ProcState::ZOMBIE
        };
        Pcb *p;
        char *state;

        printf("\n");
        for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
        {
            if (p->_state == ProcState::UNUSED)
                continue;
            if ((int)p->_state >= 0 && (int)p->_state < 6 && states[(int)p->_state])
                state = (char *)states[(int)p->_state];
            else
                state = (char *)"???";
            printf("%d %s %s", p->_pid, state, p->_name);
            printf("\n");
        }
    }

    	int ProcessManager::alloc_fd( Pcb *p, fs::file *f )
	{
		int fd;

		for ( fd = 3; fd < (int) max_open_files; fd++ )
		{
			if ( p->_ofile[fd] == nullptr )
			{
				p->_ofile[fd] = f;
				return fd;
			}
		}
		return -1;
	}
}

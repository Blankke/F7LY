#include "hal/cpu.hh"
#include "spinlock.hh"
#include "scheduler.hh"
#include "proc_manager.hh"
#include "signal.hh"
#include "printer.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
// #include "tm/timer_manager.hh"
// #include "klib/common.hh"

extern "C"
{
    extern void swtch(proc::Context *to_store, proc::Context *to_switch);
}

namespace proc
{

    Scheduler k_scheduler;

    namespace
    {
        int effective_schedule_priority(Pcb &p)
        {
            // 单核大运行队列下，显式信号目标必须尽快获得 CPU 进入用户态处理信号。
            // 这里只改变调度时的临时优先级，不改写 nice/sched_policy 等用户可见状态。
            if (ipc::signal::has_unmasked_signal_pending(&p))
            {
                return highest_proc_prio;
            }
            return p._priority;
        }
    }

    void Scheduler::init(const char *name)
    {
        _sche_lock.init(name);
        _has_non_default_priority = false;
    }

    void Scheduler::note_priority_change(int priority)
    {
        if (priority == default_proc_prio)
        {
            return;
        }
        _sche_lock.acquire();
        _has_non_default_priority = true;
        _sche_lock.release();
    }

    int Scheduler::get_highest_priority()
    {
        _sche_lock.acquire();
        if (!_has_non_default_priority)
        {
            _sche_lock.release();
            return default_proc_prio;
        }
        int prio = lowest_proc_prio;
        // _has_non_default_priority 是调度快路径标记，进程退出或策略恢复默认后需要在全表扫描时
        // 顺手清理，避免系统长期误以为存在实时任务而每轮都走优先级过滤。
        bool has_live_non_default_priority = false;
        for (Pcb &p : k_proc_pool)
        {
            if (p._state != ProcState::UNUSED &&
                p._state != ProcState::ZOMBIE &&
                p._priority != default_proc_prio)
            {
                has_live_non_default_priority = true;
            }
            // pending signal 使用临时 effective priority 参与比较，让信号目标尽快返回用户态。
            int effective_priority = effective_schedule_priority(p);
            if (effective_priority < prio && p._state == ProcState::RUNNABLE)
            {
                prio = effective_priority;
            }
        }
        if (!has_live_non_default_priority)
        {
            _has_non_default_priority = false;
        }
        _sche_lock.release();
        return prio;
    }

    void Scheduler::start_schedule()
    {
        Pcb *p;
        Cpu *cpu = Cpu::get_cpu();
        int priority;

        cpu->set_cur_proc(nullptr);

        int last_global_id = -1; // 上次调度的全局ID

        for (;;)
        {
            // printfCyan("[sche]  start_schedule here , cur_pid=%d, cur_tid=%d\n", proc::k_pm._cur_pid, proc::k_pm._cur_tid);
            cpu->interrupt_on();

            priority = get_highest_priority();

            for (p = k_proc_pool; p < &k_proc_pool[num_process]; p++)
            {
                // printfBlue("[sche]  start_schedule here,p->addr:%p \n", p);
                if (p->_state != ProcState::RUNNABLE ||
                    effective_schedule_priority(*p) > priority)
                {
                    // printf("p.global_id: %d, p.state: %d, p.name:%s not runnable or priority too high \n", p->_global_id, p->_state, p->_name);
                    continue;
                }
                // printf("p.global_id: %d, p.state: %d, p.name:%s \n", p->_global_id, p->_state, p->_name);
                p->_lock.acquire();
                if (p->get_state() == ProcState::RUNNABLE)
                {
                    p->_state = ProcState::RUNNING;
                    cpu->set_cur_proc(p);
                    proc::Context *cur_context = cpu->get_context();

                    // Debug
                    //  uint64 sp = p->get_context()->sp; // 0x0000001ffffbf000;
                    //  uint64 pa = (uint64)PTE2PA(mem::k_pagetable.kwalkaddr(sp).get_data());
                    //  printf("sp: %p, kstack: %p,pa:%p\n", sp, p->_kstack,pa);
                    //  printfCyan("[sche]  start_schedule here,p->addr:%x \n",Cpu::get_cpu()->get_cur_proc());
                    if(last_global_id != p->_global_id)
                    {
                        last_global_id = p->_global_id;
                        // printfRed("[sche]  switch to proc global_id: %d pid: %d tid: %d tgid: %d, name: %s\n", p->_global_id, p->_pid, p->_tid, p->_tgid, p->_name);
                    }
                    // printfRed("[start_schedule] cur_context:%p,sizeof context:%x, p->_context:%p,cur_pid_addr:%p\n", cur_context,sizeof(Context), &p->_context,&k_pm._cur_pid);
                    swtch(cur_context, &p->_context);
                    // printf( "return from %d, name: %s\n", p->_global_id, p->_name );
                    int refreshed_priority = get_highest_priority();
                    if (!(priority == default_proc_prio && p->_priority < priority))
                    {
                        // 若本轮调度期间出现更高优先级的可运行任务，后续扫描应立即收窄过滤门槛。
                        // 但当前任务刚从普通优先级提升到实时优先级时，保留本轮其它任务的运行机会，
                        // 避免批量初始化阶段被第一个自提升任务独占。
                        priority = refreshed_priority;
                    }
                    // bool flag = false;
                    // for (Pcb *np = k_proc_pool; np < &k_proc_pool[num_process]; np++)
                    // {
                    //     if(np->_state == ProcState::UNUSED){
                    //         flag = true;
                    //         break;
                    //     }
                    //     // printf("[sche]  proc global_id: [%d], pid: [%d], parent: [%d], state: %d, name: %s\n", np->_global_id, np->_pid,  np->get_ppid(), (int)np->_state, np->_name);
                    // }
                    // if(flag == false){
                    //     panic("no unused proc in pool, please check your code");
                    // }
                    cpu->set_cur_proc(nullptr);
                }
                p->_lock.release();
            }
        }
    }

    void Scheduler::yield()
    {
        // printfCyan("[sche]  yield here \n");
        // Cpu::get_cpu()->push_intr_off();
        Pcb *p = Cpu::get_cpu()->get_cur_proc();
        // Cpu::get_cpu()->pop_intr_off();
        // printfCyan("[sche]  yield here,p->addr:%x \n",Cpu::get_cpu()->get_cur_proc());
        p->_lock.acquire();
        // printfCyan("[sche]  yield here \n");
        p->_state = ProcState::RUNNABLE;
        call_sched(); // 注意swtch的逻辑是函数调用, 所以重新调用就是视为从这个函数返回
        p->_lock.release();
    }

    void Scheduler::call_sched()
    {
        // printfBlue("[sche]  call sched here \n");
        int intena;
        Cpu *cpu = Cpu::get_cpu();
        Cpu::get_cpu()->push_intr_off();
        Pcb *p = Cpu::get_cpu()->get_cur_proc();
        Cpu::get_cpu()->pop_intr_off();

        // assert(p->_lock.is_held(), "sched: proc lock not held");
        // assert(cpu->get_num_off() == 1, "sched: proc locks");
        // assert(p->_state != ProcState::RUNNING, "sched: proc is running");
        // assert(cpu->get_intr_stat() == false, "sched: interruptible");

        intena = cpu->get_int_ena();
        swtch(&p->_context, cpu->get_context());
        cpu->set_int_ena(intena);
    }

}

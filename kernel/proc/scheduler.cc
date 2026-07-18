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
        bool can_schedule_on_cpu(const Pcb &p, int cpu_id)
        {
            if (cpu_id < 0 || !Cpu::is_valid_cpu_id(static_cast<uint64>(cpu_id)) ||
                !p._cpu_mask.is_set(cpu_id))
            {
                return false;
            }

            // 进程池是全局的，但不能因此让每个空闲核都任意“偷取”一个未绑核
            // 任务。那会让同一控制线程在相邻时间片中无必要地跨核迁移；对于
            // 共享地址空间线程，这种迁移会放大 trapframe/TLB/内核栈切换窗口，
            // 也会破坏缓存局部性。
            //
            // 这里采用与 Starry 最近 SMP 调度调整一致的 sticky placement：
            // - 单核 affinity 任务只能在指定核运行；
            // - 多核 affinity 任务优先且仅在最近一次实际运行的、仍允许的 CPU
            //   上继续运行；
            // - affinity 变更将旧 CPU 排除后，才允许其它可用 CPU 接手，并在
            //   成功认领时更新 _last_cpu。
            //
            // 这不是对 Linux sched_setaffinity ABI 的额外限制：用户可见的
            // affinity 仍完整保存在 _cpu_mask 中；它只是调度器的默认放置策略。
            const uint64 eligible_mask = p._cpu_mask.bits & Cpu::online_cpu_mask();
            if ((eligible_mask & (1ULL << cpu_id)) == 0)
            {
                return false;
            }
            if ((eligible_mask & (eligible_mask - 1)) == 0)
            {
                return true;
            }

            const int last_cpu = p._last_cpu;
            if (last_cpu >= 0 && Cpu::is_valid_cpu_id(static_cast<uint64>(last_cpu)) &&
                (eligible_mask & (1ULL << last_cpu)) != 0)
            {
                return cpu_id == last_cpu;
            }
            return true;
        }

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
        _next_initial_cpu = -1;
        for (uint cpu_id = 0; cpu_id < NUMCPU; ++cpu_id)
        {
            _next_scan_global_id[cpu_id] = 0;
        }
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

    int Scheduler::select_initial_cpu(const CpuMask &mask, int parent_cpu)
    {
        // 所有用户任务进入 scheduler 前，主核会等待 possible CPU 全部 online；
        // 仍保留 possible mask 的兜底，以免未来早期内核线程复用该接口时因
        // online 发布窗口得到空集合。
        uint64 eligible_mask = mask.bits & Cpu::online_cpu_mask();
        if (eligible_mask == 0)
        {
            eligible_mask = mask.bits & Cpu::possible_cpu_mask();
        }
        if (eligible_mask == 0)
        {
            return parent_cpu;
        }

        _sche_lock.acquire();
        int first_cpu = _next_initial_cpu;
        if (first_cpu < 0 || !Cpu::is_valid_cpu_id(static_cast<uint64>(first_cpu)) ||
            (eligible_mask & (1ULL << first_cpu)) == 0)
        {
            // 第一个子任务先沿用父线程当前 home CPU，之后才按 CPU 编号轮转。
            // 这样 fork 的局部性和 pthread 批量创建时的铺核能力能够同时成立。
            first_cpu = parent_cpu;
            if (first_cpu < 0 || !Cpu::is_valid_cpu_id(static_cast<uint64>(first_cpu)) ||
                (eligible_mask & (1ULL << first_cpu)) == 0)
            {
                first_cpu = 0;
                while ((eligible_mask & (1ULL << first_cpu)) == 0)
                {
                    ++first_cpu;
                }
            }
        }

        int selected_cpu = first_cpu;
        for (int offset = 0; offset < NUMCPU; ++offset)
        {
            const int candidate = (first_cpu + offset) % NUMCPU;
            if ((eligible_mask & (1ULL << candidate)) != 0)
            {
                selected_cpu = candidate;
                _next_initial_cpu = (candidate + 1) % NUMCPU;
                break;
            }
        }
        _sche_lock.release();
        return selected_cpu;
    }

    int Scheduler::get_highest_priority(int cpu_id)
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
            if (effective_priority < prio && p._state == ProcState::RUNNABLE &&
                can_schedule_on_cpu(p, cpu_id))
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
        const int cpu_id = static_cast<int>(Cpu::current_cpu_id());
        int priority;

        cpu->set_cur_proc(nullptr);

        int last_global_id = -1; // 上次调度的全局ID

        for (;;)
        {

            cpu->interrupt_on();

            priority = get_highest_priority(cpu_id);

            // 使用本 CPU 的轮转扫描起点。PCB 锁仍是“任务只能同时运行在一个
            // CPU”的最终仲裁；try_acquire 失败则继续找下一项，等价于从其它核
            // 正在执行的任务旁边偷取可运行工作。
            const uint scan_begin = _next_scan_global_id[cpu_id] % num_process;
            for (uint offset = 0; offset < num_process; ++offset)
            {
                p = &k_proc_pool[(scan_begin + offset) % num_process];
                // printfBlue("[sche]  start_schedule here,p->addr:%p \n", p);
                if (p->_state != ProcState::RUNNABLE ||
                    !can_schedule_on_cpu(*p, cpu_id) ||
                    effective_schedule_priority(*p) > priority)
                {
                    // printf("p.global_id: %d, p.state: %d, p.name:%s not runnable or priority too high \n", p->_global_id, p->_state, p->_name);
                    continue;
                }
                // printf("p.global_id: %d, p.state: %d, p.name:%s \n", p->_global_id, p->_state, p->_name);
                // 运行中的任务会一直持有 PCB 锁直到主动让出 CPU。多核下若在这里
                // 自旋等待该锁，空闲核可能永远卡在进程池首项，无法挑选后续 runnable
                // 任务；因此失败时直接继续扫描，实现无全局队列的 work-stealing。
                if (!p->_lock.try_acquire())
                {
                    continue;
                }
                if (p->get_state() == ProcState::RUNNABLE && can_schedule_on_cpu(*p, cpu_id))
                {
                    // PCB 锁已经串行化了 RUNNABLE -> RUNNING 转换；再用显式
                    // owner 字段把这个 SMP 不变量固化下来。若这里失败，说明有
                    // 路径在任务尚未真正切出时错误地重新标记为了 RUNNABLE。
                    assert(p->_running_cpu == -1,
                           "scheduler: double-run pid=%d tid=%d gid=%d owner=%d claimant=%d state=%d",
                           p->_pid,
                           p->_tid,
                           p->_global_id,
                           p->_running_cpu,
                           cpu_id,
                           (int)p->_state);
                    _next_scan_global_id[cpu_id] = (p->_global_id + 1) % num_process;
                    p->_last_cpu = cpu_id;
                    p->_running_cpu = cpu_id;
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
                    swtch(cur_context, &p->_context);
                    // printf( "return from %d, name: %s\n", p->_global_id, p->_name );
                    int refreshed_priority = get_highest_priority(cpu_id);
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
        Cpu::get_cpu()->push_intr_off();
        Pcb *p = Cpu::get_cpu()->get_cur_proc();
        Cpu::get_cpu()->pop_intr_off();
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

        assert(p != nullptr, "sched: no current process");
        assert(p->_lock.is_held(), "sched: proc lock not held");
        assert(cpu->get_num_off() == 1,
               "sched: proc locks num_off=%d intr=%d intena=%d pid=%d tid=%d state=%d chan=%p name=%s",
               cpu->get_num_off(),
               cpu->get_intr_stat(),
               cpu->get_int_ena(),
               p != nullptr ? p->_pid : -1,
               p != nullptr ? p->_tid : -1,
               p != nullptr ? (int)p->_state : -1,
               p != nullptr ? p->_chan : nullptr,
               p != nullptr ? p->_name : "(null)");
        assert(p->_state != ProcState::RUNNING, "sched: proc is running");
        assert(cpu->get_intr_stat() == false, "sched: interruptible");

        // 任务仍持有自己的 PCB 锁时释放执行权。随后 swtch() 返回调度器，
        // 调度器才会释放该锁；因此其它 CPU 最早也只能在本 CPU 已经不再执行
        // 此任务之后看到 _running_cpu == -1 并重新认领它。
        assert(p->_running_cpu == static_cast<int>(Cpu::current_cpu_id()),
               "sched: owner mismatch pid=%d tid=%d gid=%d owner=%d cpu=%lu state=%d",
               p->_pid,
               p->_tid,
               p->_global_id,
               p->_running_cpu,
               Cpu::current_cpu_id(),
               (int)p->_state);
        p->_running_cpu = -1;

        intena = cpu->get_int_ena();
        swtch(&p->_context, cpu->get_context());
        cpu->set_int_ena(intena);
    }

}

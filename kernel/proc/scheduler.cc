#include "hal/cpu.hh"
#include "spinlock.hh"
#include "scheduler.hh"
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "hal/tlb_shootdown.hh"
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

        uint trailing_zero_count_nonzero(uint64 value)
        {
            // freestanding 链接不能依赖 libgcc 的 __ctzdi2；按 32/16/... 二分，
            // 固定六步得到最低置位下标。
            uint count = 0;
            if ((value & 0xffffffffULL) == 0)
            {
                count += 32;
                value >>= 32;
            }
            if ((value & 0xffffULL) == 0)
            {
                count += 16;
                value >>= 16;
            }
            if ((value & 0xffULL) == 0)
            {
                count += 8;
                value >>= 8;
            }
            if ((value & 0xfULL) == 0)
            {
                count += 4;
                value >>= 4;
            }
            if ((value & 0x3ULL) == 0)
            {
                count += 2;
                value >>= 2;
            }
            if ((value & 0x1ULL) == 0)
            {
                ++count;
            }
            return count;
        }
    }

    void Scheduler::init(const char *name)
    {
        _sche_lock.init(name);
        _non_default_schedulable.store(0, eastl::memory_order_release);
        _next_initial_cpu = -1;
        _load_averages[0] = 0;
        _load_averages[1] = 0;
        _load_averages[2] = 0;
        _load_last_sample_sec = 0;
        _load_initialized = false;
        for (uint cpu_id = 0; cpu_id < NUMCPU; ++cpu_id)
        {
            _next_scan_global_id[cpu_id] = 0;
            _pressure[cpu_id].schedulable.store(0, eastl::memory_order_relaxed);
            for (uint word_index = 0; word_index < k_runnable_slot_word_count; ++word_index)
            {
                _runnable_slot_words[cpu_id][word_index].store(
                    0, eastl::memory_order_relaxed);
            }
        }
    }

    namespace
    {
        bool valid_home_cpu(int cpu_id)
        {
            return cpu_id >= 0 && Cpu::is_valid_cpu_id(static_cast<uint64>(cpu_id));
        }
    }

    void Scheduler::set_task_state(Pcb &task, ProcState state)
    {
        const ProcState old_state = task._state;
        if (old_state == state)
        {
            return;
        }

        const bool old_runnable = old_state == ProcState::RUNNABLE;
        const bool new_runnable = state == ProcState::RUNNABLE;

        // 先撤销旧队列成员资格，再改变状态。调度器即使读到一个短暂的
        // 过期位，也会在 PCB 锁内重新检查状态；反向发布则必须先写状态、
        // 再 release 发布位图，保证新唤醒任务不会被长期遗漏。
        if (old_runnable && valid_home_cpu(task._last_cpu))
        {
            clear_runnable_slot(static_cast<uint>(task._last_cpu), task._global_id);
        }

        if (new_runnable && !valid_home_cpu(task._last_cpu))
        {
            // 所有 runnable 任务必须有唯一 home CPU，否则压力计数无法正确记账。
            task._last_cpu = select_initial_cpu(task._cpu_mask, -1);
        }

        const bool old_schedulable =
            old_state == ProcState::RUNNABLE || old_state == ProcState::RUNNING;
        const bool new_schedulable =
            state == ProcState::RUNNABLE || state == ProcState::RUNNING;
        const int home_cpu = task._last_cpu;
        if (valid_home_cpu(home_cpu) && old_schedulable != new_schedulable)
        {
            if (old_schedulable)
            {
                const uint32 previous =
                    _pressure[home_cpu].schedulable.fetch_sub(1, eastl::memory_order_acq_rel);
                assert(previous != 0,
                       "scheduler: pressure underflow cpu=%d gid=%d",
                       home_cpu,
                       task._global_id);
            }
            else
            {
                _pressure[home_cpu].schedulable.fetch_add(1, eastl::memory_order_release);
            }
        }
        if (task._priority != default_proc_prio && old_schedulable != new_schedulable)
        {
            if (old_schedulable)
            {
                const uint32 previous =
                    _non_default_schedulable.fetch_sub(1, eastl::memory_order_acq_rel);
                assert(previous != 0,
                       "scheduler: priority pressure underflow gid=%d",
                       task._global_id);
            }
            else
            {
                _non_default_schedulable.fetch_add(1, eastl::memory_order_release);
            }
        }
        task._state = state;

        if (new_runnable && valid_home_cpu(task._last_cpu))
        {
            mark_runnable_slot(static_cast<uint>(task._last_cpu), task._global_id);
        }
    }

    void Scheduler::set_task_priority(Pcb &task, int priority)
    {
        const int old_priority = task._priority;
        if (old_priority == priority)
        {
            return;
        }

        const bool schedulable =
            task._state == ProcState::RUNNABLE || task._state == ProcState::RUNNING;
        if (schedulable &&
            (old_priority == default_proc_prio) != (priority == default_proc_prio))
        {
            if (old_priority != default_proc_prio)
            {
                const uint32 previous =
                    _non_default_schedulable.fetch_sub(1, eastl::memory_order_acq_rel);
                assert(previous != 0,
                       "scheduler: priority change underflow gid=%d",
                       task._global_id);
            }
            else
            {
                _non_default_schedulable.fetch_add(1, eastl::memory_order_release);
            }
        }
        task._priority = priority;
    }

    void Scheduler::set_task_home_cpu(Pcb &task, int cpu_id)
    {
        const int old_cpu = task._last_cpu;
        if (old_cpu == cpu_id)
        {
            return;
        }

        if (task._state == ProcState::RUNNABLE || task._state == ProcState::RUNNING)
        {
            if (task._state == ProcState::RUNNABLE && valid_home_cpu(old_cpu))
            {
                clear_runnable_slot(static_cast<uint>(old_cpu), task._global_id);
            }
            if (valid_home_cpu(old_cpu))
            {
                const uint32 previous =
                    _pressure[old_cpu].schedulable.fetch_sub(1, eastl::memory_order_acq_rel);
                assert(previous != 0,
                       "scheduler: home pressure underflow cpu=%d gid=%d",
                       old_cpu,
                       task._global_id);
            }
            if (valid_home_cpu(cpu_id))
            {
                _pressure[cpu_id].schedulable.fetch_add(1, eastl::memory_order_release);
            }
        }
        task._last_cpu = cpu_id;
        if (task._state == ProcState::RUNNABLE && valid_home_cpu(cpu_id))
        {
            mark_runnable_slot(static_cast<uint>(cpu_id), task._global_id);
        }
    }

    void Scheduler::mark_runnable_slot(uint cpu_id, uint global_id)
    {
        if (cpu_id >= NUMCPU || global_id >= num_process)
        {
            return;
        }
        _runnable_slot_words[cpu_id][global_id / 64].fetch_or(
            1ULL << (global_id % 64), eastl::memory_order_release);
    }

    void Scheduler::clear_runnable_slot(uint cpu_id, uint global_id)
    {
        if (cpu_id >= NUMCPU || global_id >= num_process)
        {
            return;
        }
        _runnable_slot_words[cpu_id][global_id / 64].fetch_and(
            ~(1ULL << (global_id % 64)), eastl::memory_order_release);
    }

    uint64 Scheduler::runnable_slot_word(uint cpu_id, uint word_index) const
    {
        if (cpu_id >= NUMCPU || word_index >= k_runnable_slot_word_count)
        {
            return 0;
        }
        return _runnable_slot_words[cpu_id][word_index].load(
            eastl::memory_order_acquire);
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
        uint32 selected_pressure = ~static_cast<uint32>(0);
        for (int offset = 0; offset < NUMCPU; ++offset)
        {
            const int candidate = (first_cpu + offset) % NUMCPU;
            if ((eligible_mask & (1ULL << candidate)) == 0)
            {
                continue;
            }
            const uint32 pressure =
                _pressure[candidate].schedulable.load(eastl::memory_order_acquire);
            if (pressure < selected_pressure)
            {
                selected_cpu = candidate;
                selected_pressure = pressure;
            }
        }
        _next_initial_cpu = (selected_cpu + 1) % NUMCPU;
        _sche_lock.release();
        return selected_cpu;
    }

    uint32 Scheduler::runnable_task_count() const
    {
        uint32 total = 0;
        const uint64 online_mask = Cpu::online_cpu_mask();
        for (uint cpu_id = 0; cpu_id < NUMCPU; ++cpu_id)
        {
            if ((online_mask & (1ULL << cpu_id)) == 0)
            {
                continue;
            }
            total += _pressure[cpu_id].schedulable.load(eastl::memory_order_acquire);
        }
        return total;
    }

    void Scheduler::sample_load_averages(uint64 now_sec)
    {
        // Linux avenrun 内部使用 11 位小数并每 5 秒采样；sysinfo 再导出为
        // 16 位小数。采样由 CPU0 的 5 秒时钟点驱动，不能在 sysinfo 读取时
        // 用“当前负载”回填整段历史，否则长时间无人读取后结果会严重失真。
        constexpr uint64 fixed_1 = 1ULL << 11;
        constexpr uint64 exp_1 = 1884;
        constexpr uint64 exp_5 = 2014;
        constexpr uint64 exp_15 = 2037;
        constexpr uint64 exps[3] = {exp_1, exp_5, exp_15};
        const uint64 current = static_cast<uint64>(runnable_task_count()) * fixed_1;

        _sche_lock.acquire();
        if (!_load_initialized)
        {
            _load_averages[0] = 0;
            _load_averages[1] = 0;
            _load_averages[2] = 0;
            _load_last_sample_sec = now_sec >= 5 ? now_sec - 5 : 0;
            _load_initialized = true;
        }
        if (now_sec >= _load_last_sample_sec + 5)
        {
            uint64 samples = (now_sec - _load_last_sample_sec) / 5;
            // 正常只有一个样本；暂停/调试后最多重放到已完全收敛，防止溢出。
            if (samples > 2048)
            {
                samples = 2048;
            }
            for (uint64 sample = 0; sample < samples; ++sample)
            {
                for (uint i = 0; i < 3; ++i)
                {
                    _load_averages[i] =
                        (_load_averages[i] * exps[i] + current * (fixed_1 - exps[i])) >> 11;
                }
            }
            _load_last_sample_sec += samples * 5;
        }
        _sche_lock.release();
    }

    void Scheduler::snapshot_load_averages(uint64 loads[3])
    {
        _sche_lock.acquire();
        for (uint i = 0; i < 3; ++i)
        {
            loads[i] = _load_averages[i] << 5;
        }
        _sche_lock.release();
    }

    int Scheduler::get_highest_priority(int cpu_id)
    {
        // Cargo/rustc 的常规任务全部使用默认优先级。先做无锁判断，避免
        // 8 个 scheduler（尤其是暂时无任务的 CPU）持续争用 _sche_lock。
        if (_non_default_schedulable.load(eastl::memory_order_acquire) == 0)
        {
            return default_proc_prio;
        }

        _sche_lock.acquire();
        if (_non_default_schedulable.load(eastl::memory_order_relaxed) == 0)
        {
            _sche_lock.release();
            return default_proc_prio;
        }
        int prio = lowest_proc_prio;
        // 非默认任务计数由状态/优先级入口精确维护；这里只扫描本 CPU 的
        // runnable 位图。远端正在运行的任务由其本核继续执行，无需把“拿不
        // 到远端 PCB 锁”扩散成全局慢路径。
        for (uint word_index = 0; word_index < k_runnable_slot_word_count; ++word_index)
        {
            uint64 runnable_bits = runnable_slot_word(static_cast<uint>(cpu_id), word_index);
            while (runnable_bits != 0)
            {
                const uint bit = trailing_zero_count_nonzero(runnable_bits);
                runnable_bits &= runnable_bits - 1;
                const uint global_id = word_index * 64 + bit;
                if (global_id >= num_process)
                {
                    break;
                }

                Pcb &p = k_proc_pool[global_id];
                // 不在调度器中等待一个可能正在其它 CPU 上运行且长期持锁的任务。
                // 本轮跳过后保留全局标志，下一轮会重新检查，不会错误清除慢路径。
                // swtch 返回调度器时，当前 CPU 仍持有刚让出任务的 PCB 锁；
                // try_acquire 会按设计把同核递归加锁判为 panic，因此必须先识别。
                if (!p._lock.try_acquire_unless_held())
                {
                    continue;
                }
                const int effective_priority = effective_schedule_priority(p);
                if (effective_priority < prio && p._state == ProcState::RUNNABLE &&
                    can_schedule_on_cpu(p, cpu_id))
                {
                    prio = effective_priority;
                }
                p._lock.release();
            }
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
            bool ran_task = false;

            // 使用本 CPU runnable 位图和轮转起点。先取得 PCB 锁再读取状态
            // 与 home CPU，避免调度器对其它 CPU 正在更新的 PCB 作无锁读取。
            const uint scan_begin = _next_scan_global_id[cpu_id] % num_process;
            auto scan_runnable_range = [&](uint begin, uint end)
            {
                if (begin >= end)
                {
                    return;
                }
                const uint first_word = begin / 64;
                const uint last_word = (end - 1) / 64;
                for (uint word_index = first_word; word_index <= last_word; ++word_index)
                {
                    uint64 active_bits = runnable_slot_word(static_cast<uint>(cpu_id), word_index);
                    if (word_index == first_word)
                    {
                        active_bits &= ~0ULL << (begin % 64);
                    }
                    if (word_index == last_word && (end % 64) != 0)
                    {
                        active_bits &= (1ULL << (end % 64)) - 1;
                    }
                    while (active_bits != 0)
                    {
                        const uint bit = trailing_zero_count_nonzero(active_bits);
                        active_bits &= active_bits - 1;
                        const uint global_id = word_index * 64 + bit;
                        p = &k_proc_pool[global_id];
                        // 运行中的任务会一直持有 PCB 锁直到主动让出 CPU。失败时
                        // 直接看下一个活跃槽位，避免在其它 CPU 的任务上自旋。
                        if (!p->_lock.try_acquire())
                        {
                            continue;
                        }
                        if (p->get_state() == ProcState::RUNNABLE &&
                            can_schedule_on_cpu(*p, cpu_id) &&
                            effective_schedule_priority(*p) <= priority)
                        {
                            assert(p->_running_cpu == -1,
                                   "scheduler: double-run pid=%d tid=%d gid=%d owner=%d claimant=%d state=%d",
                                   p->_pid,
                                   p->_tid,
                                   p->_global_id,
                                   p->_running_cpu,
                                   cpu_id,
                                   (int)p->_state);
                            _next_scan_global_id[cpu_id] = (p->_global_id + 1) % num_process;
                            set_task_home_cpu(*p, cpu_id);
                            p->_running_cpu = cpu_id;
                            set_task_state(*p, ProcState::RUNNING);
                            cpu->set_cur_proc(p);
                            if (p->get_memory_manager() != nullptr)
                            {
                                hal::tlb::enter_mm(*p->get_memory_manager());
                            }
                            cpu->reset_time_slice();
                            proc::Context *cur_context = cpu->get_context();
                            if(last_global_id != p->_global_id)
                            {
                                last_global_id = p->_global_id;
                            }
                            swtch(cur_context, &p->_context);
                            ran_task = true;
                            // 退出任务已经切回 scheduler，不再使用自己的内核
                            // 栈/上下文；此时仍持有 PCB 锁，可以安全回收。
                            cpu->set_cur_proc(nullptr);
                            if (p->_state == ProcState::ZOMBIE && p->_deferred_reap)
                            {
                                k_pm.freeproc(p);
                            }
                            int refreshed_priority = get_highest_priority(cpu_id);
                            if (!(priority == default_proc_prio && p->_priority < priority))
                            {
                                priority = refreshed_priority;
                            }
                        }
                        p->_lock.release();
                    }
                }
            };
            // 保持原来的 round-robin 顺序：先扫描游标到表尾，再从表头回绕。
            scan_runnable_range(scan_begin, num_process);
            scan_runnable_range(0, scan_begin);

            if (!ran_task)
            {
                /*
                 * 没有本 CPU 可运行任务时，不再无休止扫描全局 PCB 表。
                 * 定时器每 10ms 都会唤醒 CPU；任务在扫描与 idle 之间变为
                 * RUNNABLE 的最坏调度延迟仍不超过一个 tick。
                 */
                Cpu::idle_until_interrupt();
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
        set_task_state(*p, ProcState::RUNNABLE);
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
               p != nullptr ? p->_chan.load(eastl::memory_order_acquire) : nullptr,
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
        if (p->get_memory_manager() != nullptr)
        {
            hal::tlb::leave_mm(*p->get_memory_manager());
        }

        intena = cpu->get_int_ena();
        swtch(&p->_context, cpu->get_context());
        cpu->set_int_ena(intena);
    }

}

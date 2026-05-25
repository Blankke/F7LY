#include "proc/futex.hh"
#include "time.hh"
#include "timer_manager.hh"
#include "proc/scheduler.hh"
#include "proc/proc_manager.hh"
#include "proc/proc.hh"
#include "virtual_memory_manager.hh"
#include "platform.hh"
#include "sys/syscall_defs.hh"  // 添加syscall错误码定义
#include "proc/signal.hh"       // 添加信号处理定义

namespace
{
    constexpr long k_nsec_per_sec = 1000000000L;
    SpinLock g_futex_wait_lock;
    bool g_futex_wait_lock_ready = false;

    void ensure_futex_wait_lock_ready()
    {
        if (!g_futex_wait_lock_ready)
        {
            g_futex_wait_lock.init("futex_wait_lock");
            g_futex_wait_lock_ready = true;
        }
    }

    bool fetch_robust_entry(proc::Pcb *current,
                            uint64 entry_ptr_addr,
                            proc::robust_list **entry,
                            int *pi)
    {
        unsigned long raw_entry = 0;
        if (mem::k_vmm.copy_in(*current->get_pagetable(),
                               reinterpret_cast<char *>(&raw_entry),
                               entry_ptr_addr,
                               sizeof(raw_entry)) != 0)
        {
            return false;
        }

        *entry = reinterpret_cast<proc::robust_list *>(raw_entry & ~1UL);
        *pi = static_cast<int>(raw_entry & 1UL);
        return true;
    }

    bool futex_timespec_is_valid(const tmm::timespec &ts)
    {
        return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < k_nsec_per_sec;
    }

    // 将相对超时时间转换为硬件周期数。
    // futex(FUTEX_WAIT) 的 timeout 语义是相对时间，必须按 Linux/POSIX 直接使用，
    // 不能偷偷追加额外秒数，也不能把“当前 tick 数值”当成睡眠通道。
    bool futex_timeout_to_cycles(const tmm::timespec &ts, uint64 &cycles)
    {
        if (!futex_timespec_is_valid(ts))
        {
            return false;
        }

        uint64 freq = tmm::get_main_frequence();
        uint64 sec_cycles = static_cast<uint64>(ts.tv_sec) * freq;
        uint64 nsec_cycles = (static_cast<uint64>(ts.tv_nsec) * freq) / k_nsec_per_sec;
        cycles = sec_cycles + nsec_cycles;
        return true;
    }

    int compare_timespec(const tmm::timespec &lhs, const tmm::timespec &rhs)
    {
        if (lhs.tv_sec != rhs.tv_sec)
        {
            return lhs.tv_sec < rhs.tv_sec ? -1 : 1;
        }
        if (lhs.tv_nsec != rhs.tv_nsec)
        {
            return lhs.tv_nsec < rhs.tv_nsec ? -1 : 1;
        }
        return 0;
    }
}

namespace proc
{
    void futex_sleep(void *chan, void *futex_addr)
    {
        Pcb *p = k_pm.get_cur_pcb();

        // 注意：调用者应该已经持有进程锁
        // 设置等待通道和futex地址
        p->_chan = chan;
        if (p->_futex_addr == 0)
        {
            p->_futex_addr = futex_addr;
        }
        p->_state = SLEEPING;

        k_scheduler.call_sched();

        // 清理等待通道
        p->_chan = 0;
        
        // 如果被信号唤醒，清理futex_addr以便调用者知道这是信号中断
        if (ipc::signal::has_fatal_signal_pending(p) || ipc::signal::has_unmasked_signal_pending(p)) {
            p->_futex_addr = 0;
        }
    }

    static void futex_sleep_with_interlock(void *chan, void *futex_addr, SpinLock &interlock)
    {
        Pcb *p = k_pm.get_cur_pcb();

        // futex WAIT 的“比较值”和“入睡”必须与 FUTEX_WAKE 串行化，
        // 否则 wake 可能刚好发生在两者之间，最终把等待线程永远丢在睡眠队列里。
        p->_chan = chan;
        if (p->_futex_addr == 0)
        {
            p->_futex_addr = futex_addr;
        }
        p->_state = SLEEPING;

        interlock.release();
        k_scheduler.call_sched();

        p->_chan = 0;
        if (ipc::signal::has_fatal_signal_pending(p) || ipc::signal::has_unmasked_signal_pending(p))
        {
            p->_futex_addr = 0;
        }
    }

    int futex_wait(uint64 uaddr, int val, tmm::timespec *ts,
                   bool timeout_is_absolute,
                   bool use_realtime_clock)
    {
        Pcb *p = k_pm.get_cur_pcb();
        int current_val;
        bool has_slept = false;

        ensure_futex_wait_lock_ready();
        p->_lock.acquire();

        // 处理超时等待
        if (ts)
        {
            if (!futex_timespec_is_valid(*ts))
            {
                p->_lock.release();
                return syscall::SYS_EINVAL;
            }

            uint64 deadline = 0;
            tmm::timespec absolute_deadline{};

            if (timeout_is_absolute)
            {
                absolute_deadline = *ts;
                tmm::SystemClockId clock_id = use_realtime_clock ? tmm::CLOCK_REALTIME : tmm::CLOCK_MONOTONIC;
                tmm::timespec now_ts{};
                if (tmm::k_tm.clock_gettime(clock_id, &now_ts) != 0)
                {
                    p->_lock.release();
                    return syscall::SYS_EINVAL;
                }
                if (compare_timespec(now_ts, absolute_deadline) >= 0)
                {
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }
            }
            else
            {
                uint64 timeout_cycles = 0;
                if (!futex_timeout_to_cycles(*ts, timeout_cycles))
                {
                    p->_lock.release();
                    return syscall::SYS_EINVAL;
                }

                // 零超时是合法输入，语义上应立即返回 ETIMEDOUT。
                if (timeout_cycles == 0)
                {
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }

                uint64 now = tmm::get_hw_time_stamp();
                deadline = now + timeout_cycles;
            }

            while (true)
            {
                // 检查致命信号（无法屏蔽的信号如SIGKILL）
                if (ipc::signal::has_fatal_signal_pending(p))
                {
                    // 被致命信号中断，清理状态
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_EINTR;  // 系统调用被信号中断
                }
                
                // 检查其他可中断的信号（考虑信号屏蔽）
                if (ipc::signal::has_unmasked_signal_pending(p))
                {
                    // 被可中断信号中断，清理状态
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_EINTR;  // 系统调用被信号中断
                }

                if (timeout_is_absolute)
                {
                    tmm::SystemClockId clock_id = use_realtime_clock ? tmm::CLOCK_REALTIME : tmm::CLOCK_MONOTONIC;
                    tmm::timespec now_ts{};
                    if (tmm::k_tm.clock_gettime(clock_id, &now_ts) != 0)
                    {
                        p->_futex_addr = 0;
                        p->_lock.release();
                        return syscall::SYS_EINVAL;
                    }
                    if (compare_timespec(now_ts, absolute_deadline) >= 0)
                    {
                        p->_futex_addr = 0;
                        p->_lock.release();
                        return syscall::SYS_ETIMEDOUT;
                    }
                }
                else
                {
                    uint64 now = tmm::get_hw_time_stamp();
                    if (now >= deadline)
                    {
                        p->_futex_addr = 0;
                        p->_lock.release();
                        return syscall::SYS_ETIMEDOUT;
                    }
                }

                // 让超时 futex 睡在真正会被 timer tick 唤醒的公共通道上。
                // 被 tick 唤醒后重新检查 deadline；被 futex_wakeup/信号唤醒时仍走原有分支。
                g_futex_wait_lock.acquire();
                if (mem::k_vmm.copy_in(*p->get_pagetable(),
                                       (char *)&current_val,
                                       uaddr,
                                       sizeof(int)))
                {
                    g_futex_wait_lock.release();
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_EFAULT;
                }
                if (current_val != val)
                {
                    g_futex_wait_lock.release();
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return has_slept ? 0 : syscall::SYS_EAGAIN;
                }

                futex_sleep_with_interlock(tmm::k_tm.get_tick_wait_channel(), (void *)uaddr, g_futex_wait_lock);
                has_slept = true;

                // futex_wakeup() 和“被信号打断”都会把 _futex_addr 清零。
                // 这里必须先判信号，再判正常唤醒；否则 sem_timedwait/pthread_join
                // 这类带超时或内部走 timed futex 的取消点会把 EINTR 误报成成功，
                // 用户态随后继续执行，长跑里就容易卡在取消点测例上。
                if (p->_futex_addr == 0)
                {
                    if (ipc::signal::has_fatal_signal_pending(p) ||
                        ipc::signal::has_unmasked_signal_pending(p))
                    {
                        p->_lock.release();
                        return syscall::SYS_EINTR;
                    }
                    p->_lock.release();
                    return 0;  // 成功被唤醒
                }
            }
        }

        // 无超时的等待
        // 和 timeout 路径一样，必须把“比较值”和“真正睡下去”串起来，
        // 否则 sem_wait/pthread_join 这类常见路径会偶发丢 wake。
        while (true)
        {
            g_futex_wait_lock.acquire();
            if (mem::k_vmm.copy_in(*p->get_pagetable(), (char *)&current_val, uaddr, sizeof(int)))
            {
                g_futex_wait_lock.release();
                p->_lock.release();
                return syscall::SYS_EFAULT;
            }
            if (current_val != val)
            {
                g_futex_wait_lock.release();
                p->_futex_addr = 0;
                p->_lock.release();
                return has_slept ? 0 : syscall::SYS_EAGAIN;
            }

            // 无超时等待也走 tick 通道周期性重检，避免用户态值已经变化、
            // 但由于历史库实现或竞态没有显式 FUTEX_WAKE 时永久挂死。
            futex_sleep_with_interlock(tmm::k_tm.get_tick_wait_channel(), (void *)uaddr, g_futex_wait_lock);
            has_slept = true;

            if (p->_futex_addr == 0)
            {
                if (ipc::signal::has_fatal_signal_pending(p) || ipc::signal::has_unmasked_signal_pending(p))
                {
                    p->_lock.release();
                    return syscall::SYS_EINTR;
                }
                p->_lock.release();
                return 0;
            }
        }
    }

    int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2)
    {
        ensure_futex_wait_lock_ready();

        // 参数验证
        if (val < 0)
        {
            return syscall::SYS_EINVAL;  // 无效参数
        }
        
        if (uaddr2 && (val2 < 0))
        {
            return syscall::SYS_EINVAL;  // 无效参数
        }

        // 基本的地址有效性检查（用户地址应该在用户空间范围内）
        if (uaddr == 0 || (uaddr2 && (uint64)uaddr2 == 0))
        {
            return syscall::SYS_EFAULT;  // 无效的地址
        }

        g_futex_wait_lock.acquire();
        int woken = proc::k_pm.wakeup2(uaddr, val, uaddr2, val2);
        g_futex_wait_lock.release();
        
        // wakeup2返回实际唤醒的进程数，这是成功的情况
        return woken >= 0 ? woken : syscall::SYS_ESRCH;  // 如果返回负数表示没找到进程
    }

    void futex_cleanup_robust_list(struct robust_list_head *head)
    {
        if (!head) {
            return;
        }

        Pcb *current = k_pm.get_cur_pcb();
        if (current == nullptr || current->_robust_list_user_addr == 0)
        {
            return;
        }

        // Linux robust futex ABI 里的链表指针全部是“用户虚拟地址”，并且 bit0
        // 还可能携带 PI 标记。退出清理必须始终按用户地址重新取链表，否则会把
        // 内核 alias 和用户地址混在一起，链表回到 head 时也无法正确停下。
        uint64 user_head_addr = current->_robust_list_user_addr;
        struct robust_list *user_head = reinterpret_cast<struct robust_list *>(user_head_addr);
        
        // 获取当前线程ID，用于标记mutex的原主人
        int tid = current->_tid;
        
        // 限制遍历次数，防止无限循环
        const int MAX_ROBUST_ENTRIES = 1000;
        int count = 0;

        unsigned long futex_offset = 0;
        if (mem::k_vmm.copy_in(*current->get_pagetable(),
                               reinterpret_cast<char *>(&futex_offset),
                               user_head_addr + __builtin_offsetof(robust_list_head, futex_offset),
                               sizeof(futex_offset)) != 0)
        {
            return;
        }

        struct robust_list *entry = nullptr;
        int pi = 0;
        if (!fetch_robust_entry(current,
                                user_head_addr + __builtin_offsetof(robust_list_head, list.next),
                                &entry,
                                &pi))
        {
            return;
        }

        struct robust_list *pending = nullptr;
        int pending_pi = 0;
        if (!fetch_robust_entry(current,
                                user_head_addr + __builtin_offsetof(robust_list_head, list_op_pending),
                                &pending,
                                &pending_pi))
        {
            return;
        }

        while (entry && entry != user_head && count < MAX_ROBUST_ENTRIES) {
            count++;
            struct robust_list *next_entry = nullptr;
            int next_pi = 0;
            if (!fetch_robust_entry(current,
                                    reinterpret_cast<uint64>(entry) + __builtin_offsetof(robust_list, next),
                                    &next_entry,
                                    &next_pi)) {
                printf("[futex_cleanup] Failed to read next robust_list entry\n");
                break;
            }

            // 正在加锁/摘链中的 pending 节点可能已经在链表里，Linux 会延后单独处理，
            // 避免对半更新状态的节点重复 owner-died。
            if (entry != pending)
            {
                uint64 futex_addr = reinterpret_cast<uint64>(entry) + futex_offset;
                uint32 futex_val;
                if (mem::k_vmm.copy_in(*current->get_pagetable(),
                                     reinterpret_cast<char *>(&futex_val),
                                     futex_addr,
                                     sizeof(futex_val)) == 0) {
                    if ((futex_val & FUTEX_TID_MASK) == static_cast<uint32>(tid)) {
                        uint32 new_val = (futex_val & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
                        if (mem::k_vmm.copy_out(*current->get_pagetable(),
                                              futex_addr,
                                              &new_val,
                                              sizeof(new_val)) == 0) {
                            if ((futex_val & FUTEX_WAITERS) && !pi) {
                                futex_wakeup(futex_addr, 1, nullptr, 0);
                            }
                            printf("[futex_cleanup] Cleaned robust futex at 0x%lx\n", futex_addr);
                        }
                    } else {
                        printf("[futex_cleanup] Futex at 0x%lx not owned by current thread (tid=%d, futex_val=0x%x)\n", 
                               futex_addr, tid, futex_val);
                    }
                } else {
                    printf("[futex_cleanup] Failed to read futex value at 0x%lx\n", futex_addr);
                }
            }

            entry = next_entry;
            pi = next_pi;
        }

        if (pending != nullptr)
        {
            uint64 futex_addr = reinterpret_cast<uint64>(pending) + futex_offset;
            uint32 futex_val;
            if (mem::k_vmm.copy_in(*current->get_pagetable(),
                                   reinterpret_cast<char *>(&futex_val),
                                   futex_addr,
                                   sizeof(futex_val)) == 0 &&
                (futex_val & FUTEX_TID_MASK) == static_cast<uint32>(tid))
            {
                uint32 new_val = (futex_val & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
                if (mem::k_vmm.copy_out(*current->get_pagetable(),
                                        futex_addr,
                                        &new_val,
                                        sizeof(new_val)) == 0)
                {
                    if ((futex_val & FUTEX_WAITERS) && !pending_pi)
                    {
                        futex_wakeup(futex_addr, 1, nullptr, 0);
                    }
                    printf("[futex_cleanup] Cleaned pending robust futex at 0x%lx\n", futex_addr);
                }
            }
        }
        
        if (count >= MAX_ROBUST_ENTRIES) {
            printf("[futex_cleanup] Warning: Reached maximum robust_list entries limit\n");
        }
        
        printf("[futex_cleanup] Cleaned %d robust futex entries\n", count);
    }
}

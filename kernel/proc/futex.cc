#include "proc/futex.hh"
#include "time.hh"
#include "timer_manager.hh"
#include "proc/scheduler.hh"
#include "proc/proc_manager.hh"
#include "proc/proc.hh"
#include "virtual_memory_manager.hh"
#include "hal/smp.hh"
#include "sys/syscall_defs.hh"  // 添加syscall错误码定义
#include "proc/signal.hh"       // 添加信号处理定义

namespace
{
    constexpr long k_nsec_per_sec = 1000000000L;
    constexpr uint64 k_futex_key_invalid = 0;
    constexpr uint k_futex_bucket_count = 64;
    constexpr uint k_futex_word_count = (proc::num_process + 63) / 64;

    // 每个 futex key hash 只落入一个桶。桶内位图记录 PCB global id，唤醒
    // 时只访问命中的等待者，不再在每次 WAKE 上扫描 512 个 PCB。hash 仅用于
    // 分桶；私有 futex 的完整身份仍是 (mm, uaddr)，必须在 PCB 锁内二次比较。
    struct FutexBucket
    {
        SpinLock lock;
        uint64 waiter_words[k_futex_word_count]{};
    };

    FutexBucket g_futex_buckets[k_futex_bucket_count];
    eastl::atomic<uint32> g_futex_bucket_state{0};

    void ensure_futex_buckets_ready()
    {
        constexpr uint32 k_uninitialized = 0;
        constexpr uint32 k_initializing = 1;
        constexpr uint32 k_ready = 2;
        if (g_futex_bucket_state.load(eastl::memory_order_acquire) == k_ready)
        {
            return;
        }

        uint32 expected = k_uninitialized;
        if (g_futex_bucket_state.compare_exchange_strong(
                expected, k_initializing, eastl::memory_order_acq_rel))
        {
            for (uint index = 0; index < k_futex_bucket_count; ++index)
            {
                g_futex_buckets[index].lock.init("futex_bucket_lock");
            }
            g_futex_bucket_state.store(k_ready, eastl::memory_order_release);
            return;
        }

        // 多个 pthread 可能在不同 CPU 上同时发出进程内第一个 futex；只允许
        // 一个 CPU 初始化锁对象，其余 CPU 等待发布，避免重复清零已持有的锁。
        while (g_futex_bucket_state.load(eastl::memory_order_acquire) != k_ready)
        {
            asm volatile("nop");
        }
    }

    uint futex_bucket_index_for_key(const proc::FutexKey &key)
    {
        uint64 hash = key.value;
        if (key.is_private)
        {
            hash ^= reinterpret_cast<uint64>(key.private_mm);
        }
        hash ^= hash >> 17;
        hash ^= hash >> 31;
        return static_cast<uint>(hash & (k_futex_bucket_count - 1));
    }

    FutexBucket &futex_bucket_for_key(const proc::FutexKey &key)
    {
        return g_futex_buckets[futex_bucket_index_for_key(key)];
    }

    void mark_futex_waiter_locked(FutexBucket &bucket, const proc::Pcb &p)
    {
        bucket.waiter_words[p._global_id / 64] |= 1ULL << (p._global_id % 64);
    }

    void clear_futex_waiter_locked(FutexBucket &bucket, const proc::Pcb &p)
    {
        bucket.waiter_words[p._global_id / 64] &= ~(1ULL << (p._global_id % 64));
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

        uint64 freq = tmm::clock_frequency_hz();
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

    // FUTEX_PRIVATE_FLAG 的身份必须稳定跨越 fork/COW：同一 mm 的同一用户
    // 地址就是同一个私有 futex。没有 PRIVATE_FLAG 时，MAP_SHARED/SysV SHM
    // 仍按共享物理页匹配；其余映射按 Linux 的私有地址空间语义处理。
    proc::FutexKey resolve_futex_key(proc::Pcb *p,
                                     uint64 uaddr,
                                     proc::FutexKeyScope key_scope)
    {
        proc::FutexKey key{};
        if (p == nullptr || p->get_pagetable() == nullptr || uaddr == 0)
        {
            return key;
        }

        void *pa = p->get_pagetable()->walk_addr(uaddr);
        if (pa == nullptr)
        {
            return key;
        }

        proc::ProcessMemoryManager *mm = p->get_memory_manager();
        const proc::vma *vm = mm != nullptr ? mm->find_vma_covering(uaddr) : nullptr;
        const bool use_private_key =
            key_scope == proc::FutexKeyScope::Private ||
            vm == nullptr || !vm->is_shared_mapping();
        if (use_private_key)
        {
            if (mm == nullptr)
            {
                return key;
            }
            key.value = uaddr;
            key.private_mm = mm;
            key.is_private = true;
            return key;
        }

        key.value = reinterpret_cast<uint64>(pa);
        return key;
    }

    // 桶锁内只能做无缺页复核。第一次读取已经在桶外通过 copy_in 完成；
    // 这里仅接受当前页表中已经存在的用户页，直接从内核线性映射读取，
    // 避免把缺页、COW 或 VMA 锁路径带进 futex 桶锁。
    bool load_futex_value_nofault(proc::Pcb *p, uint64 uaddr, int &value)
    {
        if (p == nullptr || p->get_pagetable() == nullptr || uaddr == 0 ||
            (uaddr & (sizeof(int) - 1)) != 0)
        {
            return false;
        }

        void *pa = p->get_pagetable()->walk_addr(uaddr);
        if (pa == nullptr)
        {
            return false;
        }

        uint64 kernel_addr = reinterpret_cast<uint64>(pa);
#ifdef LOONGARCH
        kernel_addr = to_vir(kernel_addr);
#endif
        value = *reinterpret_cast<volatile int *>(kernel_addr);
        return true;
    }

    // 把绝对时钟的剩余时间转换为硬件截止时间。绝对 CLOCK_REALTIME 发生
    // 调整时，定时器只负责提供一次唤醒机会；futex_wait 返回后仍会重新检查
    // 原始绝对时间，因此不会把时钟回拨误判成超时。
    bool futex_absolute_deadline_to_cycles(const tmm::timespec &now,
                                           const tmm::timespec &target,
                                           uint64 &deadline)
    {
        if (compare_timespec(now, target) >= 0)
        {
            return false;
        }

        tmm::timespec remaining{};
        remaining.tv_sec = target.tv_sec - now.tv_sec;
        remaining.tv_nsec = target.tv_nsec - now.tv_nsec;
        if (remaining.tv_nsec < 0)
        {
            --remaining.tv_sec;
            remaining.tv_nsec += k_nsec_per_sec;
        }

        uint64 timeout_cycles = 0;
        if (!futex_timeout_to_cycles(remaining, timeout_cycles))
        {
            return false;
        }
        // 即使剩余时间小于一个硬件周期，也要让 timekeeper 给一次重检机会。
        if (timeout_cycles == 0)
        {
            timeout_cycles = 1;
        }

        const uint64 now_cycles = tmm::get_hw_time_stamp();
        const uint64 max_uint64 = ~static_cast<uint64>(0);
        deadline = now_cycles > max_uint64 - timeout_cycles
                       ? max_uint64
                       : now_cycles + timeout_cycles;
        return true;
    }

    bool futex_buckets_ready()
    {
        return g_futex_bucket_state.load(eastl::memory_order_acquire) == 2;
    }
}

namespace proc
{
    // 以下两个辅助函数只允许在 PCB 锁保护下调用。_futex_key 的 release
    // 发布保证 value 与 _futex_private 的组合对桶内唤醒端一致可见。
    static FutexKey futex_waiter_key_locked(Pcb *p)
    {
        FutexKey key{};
        if (p == nullptr)
        {
            return key;
        }

        key.value = p->_futex_key.load(eastl::memory_order_relaxed);
        if (!key.valid())
        {
            return key;
        }
        key.is_private = p->_futex_private;
        key.private_mm = key.is_private ? p->get_memory_manager() : nullptr;
        return key;
    }

    static bool futex_waiter_matches_key_locked(Pcb *p, const FutexKey &key)
    {
        return futex_keys_equal(futex_waiter_key_locked(p), key);
    }

    static void clear_futex_waiter_key_locked(Pcb *p)
    {
        p->_futex_addr = nullptr;
        p->_futex_private = false;
        p->_futex_bucket_index.store(0, eastl::memory_order_relaxed);
        p->_futex_key.store(0, eastl::memory_order_release);
        p->_futex_timeout_deadline.store(0, eastl::memory_order_release);
    }

    static void acquire_futex_wait_interlock(Pcb *p, FutexBucket &bucket)
    {
        /*
         * futex 队列锁与 PCB 锁统一采用 interlock -> p->_lock 的顺序。
         * 调用者原本持有当前 PCB 锁；必须先释放它，再取得队列锁并重新
         * 锁住 PCB。释放窗口内发生的 WAKE 不会丢失，因为当前线程尚未
         * 发布 futex key，随后仍会在队列锁内重新读取用户值。
         */
        p->_lock.release();
        bucket.lock.acquire();
        p->_lock.acquire();
    }

    void futex_remove_waiter(Pcb *p)
    {
        if (p == nullptr)
        {
            return;
        }

        // 统一把“桶锁 -> PCB 锁”作为摘链顺序。调用方原先若持有 PCB 锁，
        // 函数返回时仍保留；否则仅在清理期间临时持有，避免与 WAKE 端形成
        // 反向锁序。
        const bool restore_pcb_lock = !p->_lock.acquire_unless_held();
        const FutexKey key = futex_waiter_key_locked(p);
        p->_lock.release();
        if (!key.valid())
        {
            p->_lock.acquire();
            clear_futex_waiter_key_locked(p);
            // 信号/退出路径也可能对一个当前不在 futex 桶中的任务调用
            // 统一清理；此时仍要撤销通用 sleep channel 的登记。
            k_pm.unregister_wait_channel(p);
            if (!restore_pcb_lock)
            {
                p->_lock.release();
            }
            return;
        }

        FutexBucket &bucket = futex_bucket_for_key(key);
        bucket.lock.acquire();
        p->_lock.acquire();
        if (futex_waiter_matches_key_locked(p, key))
        {
            clear_futex_waiter_locked(bucket, *p);
            clear_futex_waiter_key_locked(p);
            // wakeup2() 直接发布 RUNNABLE，不会经过 sleep() 返回点，因此
            // 不能把通用等待通道的摘除延后到 sleep() 尾部。
            k_pm.unregister_wait_channel(p);
        }
        bucket.lock.release();
        if (!restore_pcb_lock)
        {
            p->_lock.release();
        }
    }

    static void clear_futex_waiter(Pcb *p)
    {
        futex_remove_waiter(p);
    }

    static void futex_sleep_with_interlock(void *chan,
                                           void *futex_addr,
                                           const FutexKey &futex_key,
                                           FutexBucket &bucket)
    {
        Pcb *p = k_pm.get_cur_pcb();

        // futex WAIT 的“比较值”和“入睡”必须与 FUTEX_WAKE 串行化，
        // 否则 wake 可能刚好发生在两者之间，最终把等待线程永远丢在睡眠队列里。
        p->_futex_addr = futex_addr;
        p->_futex_private = futex_key.is_private;
        p->_futex_bucket_index.store(futex_bucket_index_for_key(futex_key),
                                     eastl::memory_order_relaxed);
        p->_futex_key.store(futex_key.value, eastl::memory_order_release);
        // 进入调度器必须只持有当前进程锁。这里复用统一 sleep 原语，
        // 它会在持有 p->_lock 后释放 futex interlock，避免手写 call_sched()
        // 时把额外锁深度带进 scheduler。
        mark_futex_waiter_locked(bucket, *p);
        p->_lock.release();
        k_pm.sleep(chan, &bucket.lock);
        // ProcessManager::sleep 的接口契约是：返回前重新取得传入的条件锁，
        // 但此时 PCB 锁已经由 sleep() 释放。因此这里先恢复 PCB 锁，再释放
        // bucket 锁，保证函数返回时与调用者约定的状态一致：只持有 p->_lock。

        p->_lock.acquire();
        bucket.lock.release();
        if (ipc::signal::has_fatal_signal_pending(p) || ipc::signal::has_unmasked_signal_pending(p))
        {
            clear_futex_waiter(p);
        }
    }

    int futex_wait(uint64 uaddr, int val, tmm::timespec *ts,
                   bool timeout_is_absolute,
                   bool use_realtime_clock,
                   FutexKeyScope key_scope)
    {
        Pcb *p = k_pm.get_cur_pcb();
        int current_val = 0;
        bool has_slept = false;

        ensure_futex_buckets_ready();
        p->_lock.acquire();
        p->_futex_timeout_deadline.store(0, eastl::memory_order_relaxed);
        p->_futex_wait_result = 0;

        auto clear_wait_state = [&]() { clear_futex_waiter(p); };

        auto load_and_resolve = [&](FutexKey &futex_key, bool nofault) -> int {
            if (nofault)
            {
                if (!load_futex_value_nofault(p, uaddr, current_val))
                {
                    return syscall::SYS_EFAULT;
                }
            }
            else if (mem::k_vmm.copy_in(*p->get_pagetable(), (char *)&current_val, uaddr, sizeof(int)))
            {
                return syscall::SYS_EFAULT;
            }
            if (current_val != val)
            {
                // 调用者根据 has_slept 把本次比较失败转换成成功返回；这里
                // 必须保留明确的失败码，不能让未写入的 futex_key 继续入桶。
                return syscall::SYS_EAGAIN;
            }
            futex_key = resolve_futex_key(p, uaddr, key_scope);
            if (!futex_key.valid())
            {
                return syscall::SYS_EFAULT;
            }
            return 0;
        };

        auto load_error_result = [&](int result) -> int {
            return result == syscall::SYS_EAGAIN && has_slept ? 0 : result;
        };

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
                    clear_wait_state();
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }
                if (!futex_absolute_deadline_to_cycles(now_ts, absolute_deadline, deadline))
                {
                    clear_wait_state();
                    p->_lock.release();
                    return syscall::SYS_EINVAL;
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
                if (timeout_cycles == 0)
                {
                    clear_wait_state();
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }
                deadline = tmm::get_hw_time_stamp() + timeout_cycles;
            }

            while (true)
            {
                if (ipc::signal::has_fatal_signal_pending(p) ||
                    ipc::signal::has_unmasked_signal_pending(p))
                {
                    clear_wait_state();
                    p->_lock.release();
                    return syscall::SYS_EINTR;
                }

                if (timeout_is_absolute)
                {
                    tmm::SystemClockId clock_id = use_realtime_clock ? tmm::CLOCK_REALTIME : tmm::CLOCK_MONOTONIC;
                    tmm::timespec now_ts{};
                    if (tmm::k_tm.clock_gettime(clock_id, &now_ts) != 0)
                    {
                        clear_wait_state();
                        p->_lock.release();
                        return syscall::SYS_EINVAL;
                    }
                    if (compare_timespec(now_ts, absolute_deadline) >= 0)
                    {
                        clear_wait_state();
                        p->_lock.release();
                        return syscall::SYS_ETIMEDOUT;
                    }
                    if (!futex_absolute_deadline_to_cycles(now_ts, absolute_deadline, deadline))
                    {
                        clear_wait_state();
                        p->_lock.release();
                        return syscall::SYS_EINVAL;
                    }
                }
                else if (tmm::get_hw_time_stamp() >= deadline)
                {
                    clear_wait_state();
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }

                // 用户值和完整 key 的第一次读取在桶锁外完成；桶锁内只做
                // 一次重新读取，避免缺页路径把一个 futex 桶拖成全局锁。
                FutexKey candidate_key{};
                int load_ret = load_and_resolve(candidate_key, false);
                if (load_ret != 0)
                {
                    clear_wait_state();
                    p->_lock.release();
                    return load_error_result(load_ret);
                }
                FutexBucket &bucket = futex_bucket_for_key(candidate_key);
                acquire_futex_wait_interlock(p, bucket);
                FutexKey futex_key{};
                load_ret = load_and_resolve(futex_key, true);
                if (load_ret != 0)
                {
                    bucket.lock.release();
                    clear_wait_state();
                    p->_lock.release();
                    return load_error_result(load_ret);
                }
                if (&futex_bucket_for_key(futex_key) != &bucket)
                {
                    bucket.lock.release();
                    p->_lock.release();
                    p->_lock.acquire();
                    continue;
                }

                p->_futex_timeout_deadline.store(deadline, eastl::memory_order_release);
                p->_futex_wait_result = 0;
                futex_sleep_with_interlock(reinterpret_cast<void *>(futex_key.value),
                                           (void *)uaddr,
                                           futex_key,
                                           bucket);
                has_slept = true;

                if (p->_futex_key.load(eastl::memory_order_acquire) == 0)
                {
                    if (ipc::signal::has_fatal_signal_pending(p) ||
                        ipc::signal::has_unmasked_signal_pending(p))
                    {
                        p->_lock.release();
                        return syscall::SYS_EINTR;
                    }
                    if (p->_futex_wait_result == syscall::SYS_ETIMEDOUT)
                    {
                        if (timeout_is_absolute)
                        {
                            tmm::SystemClockId clock_id = use_realtime_clock ? tmm::CLOCK_REALTIME : tmm::CLOCK_MONOTONIC;
                            tmm::timespec now_ts{};
                            if (tmm::k_tm.clock_gettime(clock_id, &now_ts) != 0)
                            {
                                p->_lock.release();
                                return syscall::SYS_EINVAL;
                            }
                            if (compare_timespec(now_ts, absolute_deadline) < 0)
                            {
                                // 硬件截止时间只是唤醒下界；时钟调整后仍未到
                                // 绝对截止时间时，保留原等待继续睡眠。
                                p->_futex_wait_result = 0;
                                continue;
                            }
                        }
                        p->_futex_timeout_deadline.store(0, eastl::memory_order_release);
                        p->_lock.release();
                        return syscall::SYS_ETIMEDOUT;
                    }
                    p->_lock.release();
                    return 0;
                }
            }
        }

        while (true)
        {
            FutexKey candidate_key{};
            int load_ret = load_and_resolve(candidate_key, false);
            if (load_ret != 0)
            {
                clear_wait_state();
                p->_lock.release();
                return load_error_result(load_ret);
            }
            FutexBucket &bucket = futex_bucket_for_key(candidate_key);
            acquire_futex_wait_interlock(p, bucket);
            FutexKey futex_key{};
            load_ret = load_and_resolve(futex_key, true);
            if (load_ret != 0)
            {
                bucket.lock.release();
                clear_wait_state();
                p->_lock.release();
                return load_error_result(load_ret);
            }
            if (&futex_bucket_for_key(futex_key) != &bucket)
            {
                bucket.lock.release();
                p->_lock.release();
                p->_lock.acquire();
                continue;
            }

            p->_futex_timeout_deadline.store(0, eastl::memory_order_release);
            p->_futex_wait_result = 0;

            /*
             * 无超时 FUTEX_WAIT 必须只由匹配的 FUTEX_WAKE、信号或线程组退出
             * 唤醒。旧实现把它挂到全局 tick 通道，导致所有无期限 parking
             * waiter 每个 tick 一起变为 RUNNABLE、重读用户页再睡回去，在多核
             * 高并发下形成持续惊群。
             *
             * wakeup2() 按完整 futex key 直接定位并唤醒 PCB，因此等待通道只
             * 需要是稳定的非 tick 值；使用 key value 也便于调试时辨认等待对象。
             */
            futex_sleep_with_interlock(reinterpret_cast<void *>(futex_key.value),
                                       (void *)uaddr,
                                       futex_key,
                                       bucket);
            has_slept = true;

            if (p->_futex_key.load(eastl::memory_order_acquire) == 0)
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

    void futex_check_timeouts(uint64 now_cycles)
    {
        // 第一个 futex 可能尚未初始化桶锁；不要在 timer interrupt 中做一次性
        // 初始化。之后每次 tick 都会重新检查，初始化完成后即可接管超时等待。
        if (!futex_buckets_ready())
        {
            return;
        }

        for (uint word_index = 0; word_index < k_futex_word_count; ++word_index)
        {
            uint64 candidates = k_pm.active_slot_word(word_index);
            while (candidates != 0)
            {
                // 不直接使用 __builtin_ctzll：当前 freestanding 链接不提供
                // 编译器可能生成的 __ctzdi2，手动取最低位避免引入运行库依赖。
                uint bit = 0;
                uint64 bit_probe = candidates;
                while ((bit_probe & 1ULL) == 0)
                {
                    bit_probe >>= 1;
                    ++bit;
                }
                candidates &= candidates - 1;
                const uint global_id = word_index * 64 + bit;
                if (global_id >= num_process)
                {
                    continue;
                }

                Pcb *p = &k_proc_pool[global_id];
                if (p == k_pm.get_cur_pcb())
                {
                    continue;
                }

                const uint64 deadline =
                    p->_futex_timeout_deadline.load(eastl::memory_order_acquire);
                if (deadline == 0 || now_cycles < deadline)
                {
                    continue;
                }

                const uint64 candidate_key =
                    p->_futex_key.load(eastl::memory_order_acquire);
                if (candidate_key == k_futex_key_invalid)
                {
                    continue;
                }

                const uint32 candidate_bucket_index =
                    p->_futex_bucket_index.load(eastl::memory_order_acquire);
                if (candidate_bucket_index >= k_futex_bucket_count)
                {
                    continue;
                }

                FutexBucket &bucket = g_futex_buckets[candidate_bucket_index];
                bucket.lock.acquire();
                p->_lock.acquire();

                int wake_cpu = -1;
                const uint64 observed_key =
                    p->_futex_key.load(eastl::memory_order_relaxed);
                const uint64 observed_deadline =
                    p->_futex_timeout_deadline.load(eastl::memory_order_relaxed);
                const uint32 observed_bucket_index =
                    p->_futex_bucket_index.load(eastl::memory_order_relaxed);
                const bool expired_wait =
                    observed_key == candidate_key &&
                    observed_bucket_index == candidate_bucket_index &&
                    observed_deadline != 0 && now_cycles >= observed_deadline &&
                    p->_state == ProcState::SLEEPING &&
                    p->_chan.load(eastl::memory_order_relaxed) ==
                        reinterpret_cast<void *>(candidate_key);
                if (expired_wait)
                {
                    clear_futex_waiter_locked(bucket, *p);
                    clear_futex_waiter_key_locked(p);
                    p->_futex_wait_result = syscall::SYS_ETIMEDOUT;
                    k_pm.unregister_wait_channel(p);
                    k_scheduler.set_task_state(*p, ProcState::RUNNABLE);
                    wake_cpu = p->_last_cpu;
                }

                p->_lock.release();
                bucket.lock.release();
                if (wake_cpu >= 0 && wake_cpu != static_cast<int>(Cpu::current_cpu_id()))
                {
                    hal::smp::kick_cpu(static_cast<uint64>(wake_cpu));
                }
            }
        }
    }

    int futex_wakeup(uint64 uaddr,
                     int val,
                     void *uaddr2,
                     int val2,
                     FutexKeyScope key_scope,
                     bool include_requeued_in_result,
                     const int *expected_requeue_value)
    {
        ensure_futex_buckets_ready();

        if (val < 0)
        {
            return syscall::SYS_EINVAL;
        }
        if (uaddr2 && val2 < 0)
        {
            return syscall::SYS_EINVAL;
        }
        if (uaddr == 0 || (uaddr2 && (uint64)uaddr2 == 0))
        {
            return syscall::SYS_EFAULT;
        }

        Pcb *cur = k_pm.get_cur_pcb();
        FutexKey futex_key = resolve_futex_key(cur, uaddr, key_scope);
        if (!futex_key.valid())
        {
            return syscall::SYS_EFAULT;
        }

        FutexKey futex_key2{};
        if (uaddr2 != nullptr)
        {
            futex_key2 = resolve_futex_key(cur,
                                            reinterpret_cast<uint64>(uaddr2),
                                            key_scope);
            if (!futex_key2.valid())
            {
                return syscall::SYS_EFAULT;
            }
        }

        FutexBucket &source_bucket = futex_bucket_for_key(futex_key);
        const uint source_bucket_index = futex_bucket_index_for_key(futex_key);
        FutexBucket *target_bucket = uaddr2 != nullptr
                                         ? &futex_bucket_for_key(futex_key2)
                                         : nullptr;
        const uint target_bucket_index = uaddr2 != nullptr
                                             ? futex_bucket_index_for_key(futex_key2)
                                             : source_bucket_index;
        if (target_bucket == nullptr || target_bucket == &source_bucket)
        {
            source_bucket.lock.acquire();
        }
        else if (target_bucket_index < source_bucket_index)
        {
            target_bucket->lock.acquire();
            source_bucket.lock.acquire();
        }
        else
        {
            source_bucket.lock.acquire();
            target_bucket->lock.acquire();
        }

        // CMP_REQUEUE 的 val3 比较必须和 source bucket 的队列迁移处于同一
        // 临界区。若在 syscall 分发层先比较、随后才取桶锁，另一颗 CPU 可以
        // 在两者之间改写 uaddr，导致本应 EAGAIN 的条件变量等待者被错误迁移。
        if (expected_requeue_value != nullptr)
        {
            int observed_value = 0;
            const bool value_loaded =
                load_futex_value_nofault(cur, uaddr, observed_value);
            if (!value_loaded || observed_value != *expected_requeue_value)
            {
                if (target_bucket != nullptr && target_bucket != &source_bucket)
                {
                    target_bucket->lock.release();
                }
                source_bucket.lock.release();
                return value_loaded ? syscall::SYS_EAGAIN : syscall::SYS_EFAULT;
            }
        }

        int woken = proc::k_pm.wakeup2(
            futex_key,
            val,
            uaddr2,
            futex_key2,
            val2,
            source_bucket.waiter_words,
            target_bucket != nullptr ? target_bucket->waiter_words : nullptr,
            k_futex_word_count,
            target_bucket_index,
            include_requeued_in_result);

        if (target_bucket != nullptr && target_bucket != &source_bucket)
        {
            target_bucket->lock.release();
        }
        source_bucket.lock.release();
        return woken >= 0 ? woken : syscall::SYS_ESRCH;
    }

    int futex_wake_op(uint64 uaddr1,
                      int wake_count,
                      uint64 uaddr2,
                      int second_wake_count,
                      uint32 encoded_op,
                      FutexKeyScope key_scope)
    {
        if (uaddr1 == 0 || uaddr2 == 0 || wake_count < 0 || second_wake_count < 0)
        {
            return syscall::SYS_EINVAL;
        }

        Pcb *current = k_pm.get_cur_pcb();
        if (current == nullptr || current->get_pagetable() == nullptr)
        {
            return syscall::SYS_ESRCH;
        }

        // Linux FUTEX_WAKE_OP 的编码为：操作类型[30:28]、比较类型[27:24]、
        // 操作数[23:12]、比较值[11:0]；bit31 表示操作数是 1 << shift。
        // 这里先完成第二个 futex 地址上的 32 位读改写，再按旧值比较结果
        // 分别唤醒两个 futex 队列，不能把 WAKE_OP 错当成 REQUEUE。
        int old_value = 0;
        if (mem::k_vmm.copy_in(*current->get_pagetable(),
                               reinterpret_cast<char *>(&old_value),
                               uaddr2,
                               sizeof(old_value)) < 0)
        {
            return syscall::SYS_EFAULT;
        }

        const uint32 raw = encoded_op;
        const uint op = (raw >> 28) & 0x7U;
        const uint cmp = (raw >> 24) & 0xFU;
        uint32 operation_argument = (raw >> 12) & 0xFFFU;
        const int comparison_argument = static_cast<int>(raw & 0xFFFU);
        if ((raw & 0x80000000U) != 0)
        {
            if (operation_argument >= 31)
            {
                return syscall::SYS_EINVAL;
            }
            operation_argument = 1U << operation_argument;
        }

        int new_value = old_value;
        switch (op)
        {
        case 0: // FUTEX_OP_SET
            new_value = static_cast<int>(operation_argument);
            break;
        case 1: // FUTEX_OP_ADD
            new_value = old_value + static_cast<int>(operation_argument);
            break;
        case 2: // FUTEX_OP_OR
            new_value = old_value | static_cast<int>(operation_argument);
            break;
        case 3: // FUTEX_OP_ANDN
            new_value = old_value & ~static_cast<int>(operation_argument);
            break;
        case 4: // FUTEX_OP_XOR
            new_value = old_value ^ static_cast<int>(operation_argument);
            break;
        default:
            return syscall::SYS_ENOSYS;
        }

        if (mem::k_vmm.copy_out(*current->get_pagetable(),
                                uaddr2,
                                &new_value,
                                sizeof(new_value)) < 0)
        {
            return syscall::SYS_EFAULT;
        }

        bool compare_matches = false;
        switch (cmp)
        {
        case 0: compare_matches = old_value == comparison_argument; break; // EQ
        case 1: compare_matches = old_value != comparison_argument; break; // NE
        case 2: compare_matches = old_value < comparison_argument; break;  // LT
        case 3: compare_matches = old_value <= comparison_argument; break; // LE
        case 4: compare_matches = old_value > comparison_argument; break;  // GT
        case 5: compare_matches = old_value >= comparison_argument; break;  // GE
        default: return syscall::SYS_ENOSYS;
        }

        int result = futex_wakeup(uaddr1, wake_count, nullptr, 0, key_scope);
        if (result < 0)
        {
            return result;
        }
        if (compare_matches)
        {
            int second_result = futex_wakeup(uaddr2,
                                              second_wake_count,
                                              nullptr,
                                              0,
                                              key_scope);
            if (second_result < 0)
            {
                return second_result;
            }
            result += second_result;
        }
        return result;
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
                        }
                    }
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
                }
            }
        }
    }
}

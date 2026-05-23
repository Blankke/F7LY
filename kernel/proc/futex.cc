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

    // 将相对超时时间转换为硬件周期数。
    // futex(FUTEX_WAIT) 的 timeout 语义是相对时间，必须按 Linux/POSIX 直接使用，
    // 不能偷偷追加额外秒数，也不能把“当前 tick 数值”当成睡眠通道。
    bool futex_timeout_to_cycles(const tmm::timespec &ts, uint64 &cycles)
    {
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= k_nsec_per_sec)
        {
            return false;
        }

        uint64 freq = tmm::get_main_frequence();
        uint64 sec_cycles = static_cast<uint64>(ts.tv_sec) * freq;
        uint64 nsec_cycles = (static_cast<uint64>(ts.tv_nsec) * freq) / k_nsec_per_sec;
        cycles = sec_cycles + nsec_cycles;
        return true;
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

    int futex_wait(uint64 uaddr, int val, tmm::timespec *ts)
    {
        Pcb *p = k_pm.get_cur_pcb();
        int current_val;

        p->_lock.acquire();

        // 检查用户地址并读取当前值 - 内存访问错误
        if (mem::k_vmm.copy_in(*p->get_pagetable(), (char *)&current_val, uaddr, sizeof(int)))
        {
            p->_lock.release();
            return syscall::SYS_EFAULT;  // 无效的用户空间地址
        }

        // 如果值不匹配，直接返回 - 这不是错误，是正常的futex语义
        if (current_val != val)
        {
            printf("[futex_wait] current_val: %d val: %d\n", current_val, val);
            p->_lock.release();
            return syscall::SYS_EAGAIN;  // 值已改变，资源暂时不可用
        }

        // 处理超时等待
        if (ts)
        {
            uint64 timeout_cycles = 0;
            if (!futex_timeout_to_cycles(*ts, timeout_cycles))
            {
                p->_lock.release();
                return syscall::SYS_EINVAL;
            }

            uint64 now = tmm::get_hw_time_stamp();
            uint64 deadline = now + timeout_cycles;

            // 零超时是合法输入，语义上应立即返回 ETIMEDOUT。
            if (timeout_cycles == 0)
            {
                p->_futex_addr = 0;
                p->_lock.release();
                return syscall::SYS_ETIMEDOUT;
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

                now = tmm::get_hw_time_stamp();
                if (now >= deadline)
                {
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_ETIMEDOUT;
                }

                // 让超时 futex 睡在真正会被 timer tick 唤醒的公共通道上。
                // 被 tick 唤醒后重新检查 deadline；被 futex_wakeup/信号唤醒时仍走原有分支。
                futex_sleep(tmm::k_tm.get_tick_wait_channel(), (void *)uaddr);

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
        // futex_sleep会管理锁的释放和重新获取，并会检查信号
        futex_sleep((void *)uaddr, (void *)uaddr);

        // 被唤醒后检查状态并释放锁
        if (p->_futex_addr == 0)
        {
            // 检查是否是因为信号而被清零
            if (ipc::signal::has_fatal_signal_pending(p) || ipc::signal::has_unmasked_signal_pending(p))
            {
                p->_lock.release();
                return syscall::SYS_EINTR;  // 被信号中断
            }
            
            // 正常唤醒（被futex_wakeup唤醒）
            p->_lock.release();
            return 0;  // 成功
        }
        else
        {
            // 异常情况：futex_addr没有被清零，可能是spurious wakeup
            // 清理状态
            p->_futex_addr = 0;
            p->_lock.release();
            return syscall::SYS_EINTR;  // 异常唤醒，当作中断处理
        }
    }

    int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2)
    {
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

        int woken = proc::k_pm.wakeup2(uaddr, val, uaddr2, val2);
        
        // wakeup2返回实际唤醒的进程数，这是成功的情况
        return woken >= 0 ? woken : syscall::SYS_ESRCH;  // 如果返回负数表示没找到进程
    }

    void futex_cleanup_robust_list(struct robust_list_head *head)
    {
        if (!head) {
            return;
        }

        Pcb *current = k_pm.get_cur_pcb();
        
        // 获取当前线程ID，用于标记mutex的原主人
        int tid = current->_tid;
        
        // 限制遍历次数，防止无限循环
        const int MAX_ROBUST_ENTRIES = 1000;
        int count = 0;
        
        // 首先处理list_op_pending中可能正在操作的锁
        if (head->list_op_pending && head->list_op_pending != &head->list) {
            // 计算futex地址：robust_list地址 + futex_offset
            uint64 futex_addr = (uint64)head->list_op_pending + head->futex_offset;
            
            // 尝试读取并修改futex值
            uint32 futex_val;
            if (mem::k_vmm.copy_in(*current->get_pagetable(), 
                                 (char *)&futex_val, futex_addr, sizeof(futex_val)) == 0) {
                
                // 检查这个futex是否真的属于当前线程
                if ((futex_val & FUTEX_TID_MASK) == (uint32)tid) {
                    // 标记owner died，保留waiters bit
                    uint32 new_val = (futex_val & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
                    
                    if (mem::k_vmm.copy_out(*current->get_pagetable(), 
                                          futex_addr, &new_val, sizeof(new_val)) == 0) {
                        // 如果有等待者，唤醒它们
                        if (futex_val & FUTEX_WAITERS) {
                            futex_wakeup(futex_addr, 1, nullptr, 0);
                        }
                        printf("[futex_cleanup] Cleaned pending robust futex at 0x%lx\n", futex_addr);
                    }
                }
            }
        }
        
        // 遍历robust_list链表
        struct robust_list *entry = head->list.next;
        
        while (entry && entry != &head->list && count < MAX_ROBUST_ENTRIES) {
            count++;
            
            // 计算futex地址
            uint64 futex_addr = (uint64)entry + head->futex_offset;
            
            // 读取下一个entry，因为我们可能会修改当前entry
            struct robust_list *next_entry = nullptr;
            if (mem::k_vmm.copy_in(*current->get_pagetable(), 
                                 (char *)&next_entry, (uint64)&entry->next, sizeof(next_entry)) != 0) {
                printf("[futex_cleanup] Failed to read next robust_list entry\n");
                break;
            }
            
            // 尝试读取并修改futex值
            uint32 futex_val;
            if (mem::k_vmm.copy_in(*current->get_pagetable(), 
                                 (char *)&futex_val, futex_addr, sizeof(futex_val)) == 0) {
                
                // 检查这个futex是否真的属于当前线程
                if ((futex_val & FUTEX_TID_MASK) == (uint32)tid) {
                    // 标记owner died，保留waiters bit
                    uint32 new_val = (futex_val & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
                    
                    if (mem::k_vmm.copy_out(*current->get_pagetable(), 
                                          futex_addr, &new_val, sizeof(new_val)) == 0) {
                        // 如果有等待者，唤醒它们
                        if (futex_val & FUTEX_WAITERS) {
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
            
            // 移动到下一个entry
            entry = next_entry;
        }
        
        if (count >= MAX_ROBUST_ENTRIES) {
            printf("[futex_cleanup] Warning: Reached maximum robust_list entries limit\n");
        }
        
        printf("[futex_cleanup] Cleaned %d robust futex entries\n", count);
    }
}

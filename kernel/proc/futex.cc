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
namespace proc
{
    // 辅助函数：检查是否有未被屏蔽的致命信号需要处理
    static bool has_fatal_signal_pending(Pcb *p)
    {
        if (p->_signal == 0) {
            return false; // 没有待处理信号
        }
        
        // 检查致命信号（无法被屏蔽的信号）
        // SIGKILL 和 SIGSTOP 无法被屏蔽或忽略
        using namespace ipc::signal;
        return (p->_signal & (1ULL << (SIGKILL - 1))) != 0 ||
               (p->_signal & (1ULL << (SIGSTOP - 1))) != 0;
    }
    
    // 辅助函数：检查是否有可中断的信号需要处理
    static bool has_interruptible_signal_pending(Pcb *p)
    {
        if (p->_signal == 0) {
            return false; // 没有待处理信号
        }
        
        // 检查所有未被屏蔽的信号
        uint64 unmasked_signals = p->_signal & ~p->_sigmask;
        return unmasked_signals != 0;
    }

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
        if (has_fatal_signal_pending(p) || has_interruptible_signal_pending(p)) {
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
            uint64 n;
            n = (ts->tv_sec + 3) * tmm::qemu_fre + (ts->tv_nsec * tmm::qemu_fre) / 1000000000;
            uint64 timestamp;
            timestamp = rdtime();

            while (rdtime() - timestamp < n)
            {
                // 检查致命信号（无法屏蔽的信号如SIGKILL）
                if (has_fatal_signal_pending(p))
                {
                    // 被致命信号中断，清理状态
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_EINTR;  // 系统调用被信号中断
                }
                
                // 检查其他可中断的信号（考虑信号屏蔽）
                if (has_interruptible_signal_pending(p))
                {
                    // 被可中断信号中断，清理状态
                    p->_futex_addr = 0;
                    p->_lock.release();
                    return syscall::SYS_EINTR;  // 系统调用被信号中断
                }

                // futex_sleep会管理锁的释放和重新获取
                futex_sleep((void *)tmm::k_tm.get_ticks(), (void *)uaddr);

                // 检查是否被正常唤醒（futex_addr被清零表示正常唤醒）
                if (p->_futex_addr == 0)
                {
                    p->_lock.release();
                    return 0;  // 成功被唤醒
                }
            }

            // 超时处理：清理状态并返回
            p->_futex_addr = 0;
            p->_lock.release();
            return syscall::SYS_ETIMEDOUT;  // 操作超时
        }

        // 无超时的等待
        // futex_sleep会管理锁的释放和重新获取，并会检查信号
        futex_sleep((void *)uaddr, (void *)uaddr);

        // 被唤醒后检查状态并释放锁
        if (p->_futex_addr == 0)
        {
            // 检查是否是因为信号而被清零
            if (has_fatal_signal_pending(p) || has_interruptible_signal_pending(p))
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
}
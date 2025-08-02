#include "proc/futex.hh"
#include "time.hh"
#include "timer_manager.hh"
#include "proc/scheduler.hh"
#include "proc/proc_manager.hh"
#include "proc/proc.hh"
#include "virtual_memory_manager.hh"
#include "platform.hh"
namespace proc
{

    void futex_sleep(void *chan, void *futex_addr)
    {
        Pcb *p = k_pm.get_cur_pcb();

        p->_chan = chan;
        if (p->_futex_addr == 0)
        {
            p->_futex_addr = futex_addr;
        }
        p->_state = SLEEPING;

        k_scheduler.call_sched();

        // printf("%d\n", p->state);
        p->_chan = 0;
    }

    int futex_wait(uint64 uaddr, int val, tmm::timespec *ts)
    {
        Pcb *p = k_pm.get_cur_pcb();
        int current_val;

        p->_lock.acquire();
        if (mem::k_vmm.copy_in(*p->get_pagetable(), (char *)&current_val, uaddr, sizeof(int)))
        {
                p->_lock.release();
                return -1;
        }

        if (current_val != val)
        {
            // printf("%d %d\n", current_val, val);
            p->_lock.release();
            return -1;
        }

        // printf("11111\n");

        if (ts)
        {
            uint64 n;
            n = (ts->tv_sec + 3) * tmm::qemu_fre+ (ts->tv_nsec * tmm::qemu_fre) / 1000000000;
            uint64 timestamp;
            timestamp = rdtime();
            while (rdtime() - timestamp < n)
            {
                if (p->_signal == 9)
                {
                    p->_lock.release();
                    return -1;
                }
                futex_sleep((void*)tmm::k_tm.get_ticks(), (void *)uaddr);
                if (p->_futex_addr == 0)
                {
                    p->_lock.release();
                    return 0;
                }
            }
            p->_futex_addr = 0;
            p->_lock.release();
            return 0;
        }
        futex_sleep((void *)uaddr, (void *)uaddr);
        p->_lock.release();
        return 0;
    }

    int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2)
    {

        return proc::k_pm.wakeup2(uaddr, val, uaddr2, val2);
    }
}
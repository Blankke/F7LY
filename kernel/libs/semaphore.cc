
#include "semaphore.hh"
#include "proc_manager.hh"
#include "scheduler.hh"
#include "proc.hh"
void sem_init(sem *s, int value, char *name) {
    s->value = value;
    s->top = 0;
    s->wakeup = 0;
    s->lock.init( name);
    for (int i = 0; i < NPROC; i++) {
        s->wait_list[i] = 0;
    }
}
/*
 *
 *这里的PV操作比较暴力，将wait_list全部唤醒，待优化
 *TODO：
 *应该用信号来进行唤醒
 */
void sem_p(sem *s) {
    s->lock.acquire();
    if (s->value == 0) {
        do {
            proc::Pcb *p =proc::k_pm.get_cur_pcb();
            p->_lock.acquire();
            s->wait_list[s->top++] = proc::k_pm.get_cur_pcb();
            s->wait_list[s->top - 1]->_state = proc::SLEEPING;
            s->lock.release();
            proc::k_scheduler.call_sched();
            p->_lock.release();
            if (p->_killed) {
                proc::k_pm.exit(-1);
            }
            s->lock.acquire();
        } while (s->wakeup == 0);
        s->wakeup --;
    }
    s->lock.release();
}

void sem_v(sem *s) {
    s->lock.acquire();
    s->value++;
    if (s->value <= 0) {
        s->wakeup ++;
        for (int i=0;i<s->top;i++) {
            s->wait_list[i]->
            _state = proc::RUNNABLE;
        }
        s->top = 0;
    }
    s->lock.release();
}





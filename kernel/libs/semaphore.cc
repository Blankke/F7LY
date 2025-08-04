
#include "semaphore.hh"
#include "proc_manager.hh"
#include "scheduler.hh"
#include "proc.hh"

void sem_init(sem *s, int value, char *name)
{
    s->value = value;
    s->max_value = -1;  // 无最大值限制
    s->top = 0;
    s->lock.init(name);
    for (int i = 0; i < NPROC; i++)
    {
        s->wait_list[i] = 0;
    }
}

void sem_init_with_max(sem *s, int value, int max_value, char *name)
{
    s->value = value;
    s->max_value = max_value;
    s->top = 0;
    s->lock.init(name);
    for (int i = 0; i < NPROC; i++)
    {
        s->wait_list[i] = 0;
    }
}
/*
 * P操作：获取信号量
 * 如果信号量值大于0，则递减并继续执行
 * 如果信号量值为0，则阻塞等待
 */
void sem_p(sem *s)
{
    s->lock.acquire();
    s->value--; // 先减少计数器
    if (s->value < 0)
    {
        // 需要等待
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        p->_lock.acquire();
        s->wait_list[s->top++] = p;
        p->_state = proc::SLEEPING;
        s->lock.release();
        proc::k_scheduler.call_sched();
        p->_lock.release();
        if (p->_killed)
        {
            proc::k_pm.exit(-1);
        }
    }
    else
    {
        s->lock.release();
    }
}

/*
 * V操作：释放信号量
 * 如果有等待进程，优先唤醒等待进程
 * 否则递增信号量值，但不能超过最大值限制
 */
void sem_v(sem *s)
{
    s->lock.acquire();
    
    if (s->top > 0) // 如果有等待的进程，优先唤醒
    {
        // 唤醒一个等待的进程，不增加value（因为进程会消费这个信号量）
        s->wait_list[0]->_state = proc::RUNNABLE;
        
        // 将等待队列向前移动
        for (int i = 0; i < s->top - 1; i++)
        {
            s->wait_list[i] = s->wait_list[i + 1];
        }
        s->top--;
        s->value++; // 从负数恢复到正确的值
    }
    else
    {
        // 没有等待进程，检查是否达到最大值限制
        if (s->max_value > 0 && s->value >= s->max_value)
        {
            s->lock.release();
            return; // 达到最大值，不能再释放
        }
        s->value++;
    }
    
    s->lock.release();
}

/*
 * 非阻塞P操作：尝试获取信号量
 * 成功返回true，失败返回false
 */
bool sem_try_p(sem *s)
{
    s->lock.acquire();
    if (s->value > 0)
    {
        s->value--;
        s->lock.release();
        return true;
    }
    s->lock.release();
    return false;
}

/*
 * 非阻塞V操作：尝试释放信号量
 * 成功返回true，失败返回false（达到最大值限制）
 */
bool sem_try_v(sem *s)
{
    s->lock.acquire();
    
    if (s->top > 0) // 如果有等待的进程，优先唤醒
    {
        // 唤醒一个等待的进程，不增加value（因为进程会消费这个信号量）
        s->wait_list[0]->_state = proc::RUNNABLE;
        
        // 将等待队列向前移动
        for (int i = 0; i < s->top - 1; i++)
        {
            s->wait_list[i] = s->wait_list[i + 1];
        }
        s->top--;
        s->value++; // 从负数恢复到正确的值
        s->lock.release();
        return true;
    }
    else
    {
        // 没有等待进程，检查是否达到最大值限制
        if (s->max_value > 0 && s->value >= s->max_value)
        {
            s->lock.release();
            return false; // 达到最大值，不能再释放
        }
        s->value++;
        s->lock.release();
        return true;
    }
}

/*
 * 辅助函数：获取当前信号量值
 */
int sem_get_value(sem *s)
{
    s->lock.acquire();
    int value = s->value;
    s->lock.release();
    return value;
}

/*
 * 辅助函数：获取最大值限制
 * 返回-1表示无限制
 */
int sem_get_max_value(sem *s)
{
    s->lock.acquire();
    int max_value = s->max_value;
    s->lock.release();
    return max_value;
}

/*
 * 辅助函数：获取等待进程数量
 */
int sem_get_waiting_count(sem *s)
{
    s->lock.acquire();
    int waiting_count = s->top;
    s->lock.release();
    return waiting_count;
}

#pragma once
#include "types.hh"
#include "spinlock.hh"
#include "param.h"
#include "platform.hh"
#include "proc/proc.hh"

// 如果项目中没有bool类型，则定义它
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif

typedef struct semaphore {
    volatile int value;        // 当前信号量值
    volatile int max_value;    // 最大值限制，-1表示无限制
    SpinLock lock;
    /*
     *等待队列
     *TODO：
     *用动态内存分配，用QUEUE
     */
    proc::Pcb* wait_list[NPROC];
    int top;
} sem;


void sem_init(sem *s, int value, char *name);
void sem_init_with_max(sem *s, int value, int max_value, char *name);

void sem_p(sem *s);
void sem_v(sem *s);

// 非阻塞版本，用于支持超时等待
bool sem_try_p(sem *s);  // 尝试获取信号量，成功返回true，失败返回false
bool sem_try_v(sem *s);  // 尝试释放信号量，成功返回true，失败返回false（达到最大值）

// 辅助函数
int sem_get_value(sem *s);      // 获取当前信号量值
int sem_get_max_value(sem *s);  // 获取最大值限制（-1表示无限制）
int sem_get_waiting_count(sem *s); // 获取等待进程数量





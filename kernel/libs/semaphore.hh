#pragma once
#include "types.hh"
#include "spinlock.hh"
#include "param.h"
#include "platform.hh"
#include "proc/proc.hh"

typedef struct semaphore {
    volatile int value;
    volatile int wakeup;
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

void sem_p(sem *s);

void sem_v(sem *s);





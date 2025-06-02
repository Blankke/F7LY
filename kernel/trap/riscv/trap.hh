#pragma once
#include "types.hh"
#include "devs/spinlock.hh"
namespace tmm
{
    class TimerManager;
}

class trap_manager
{
friend  class tmm::TimerManager;
public:
    void init();
    void inithart();

    // void trapframe_dump(struct trapframe *tf); // 调试工具

    int devintr();// 处理外部中断和软件中断

    void usertrap();    // 用户态中断处理
    void usertrapret(); // 用户态返回处理

    void kerneltrap();  // 内核态中断处理
private:
    // void syscall();     // 系统调用处理
    void timertick();   // 时钟中断处理
    void set_next_timeout(); // 设置下次超时

    SpinLock tickslock; // 保护ticks的自旋锁
    uint ticks;        // 时钟中断计数
    uint timeslice;    // 时间片计数
};

extern trap_manager trap_mgr; // 创建一个静态的对象, 用于全局访问(其实相当于面向过程, 只是封装了一下)

#pragma once

class plic_manager
{
public:
    void init();
    void inithart();
    int claim();
    void complete(int irq);
};

extern plic_manager plic_mgr; // 创建一个静态的对象, 用于全局访问(其实相当于面向过程, 只是封装了一下)
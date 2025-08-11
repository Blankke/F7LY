#pragma once

#include "types.hh"
#include "tm/timer_manager.hh"

// 扩展的定时器结构体定义
struct extended_posix_timer
{
    int timer_id;               // 定时器 ID
    int clockid;                // 时钟类型
    struct sigevent
    {
        int sigev_notify;
        int sigev_signo;
        union sigval
        {
            int sival_int;
            void *sival_ptr;
        } sigev_value;
    } event;                    // 事件配置
    bool active;                // 是否激活
    bool armed;                 // 是否武装（设置了过期时间）
    tmm::itimerspec spec;       // 定时器规格
    tmm::timespec expiry_time;  // 绝对过期时间
};

// 全局定时器数组的外部声明
extern extended_posix_timer g_timers[32];
extern int g_next_timer_id;
extern bool g_timers_initialized;

// 检查过期定时器的函数声明
void check_expired_timers();

#pragma once

#include "types.hh"
#include "tm/timer_manager.hh"

namespace proc
{
    class Pcb;

    struct interval_timer_snapshot
    {
        uint64 value_us;
        uint64 interval_us;
    };

    bool is_valid_interval_timer_kind(int which);
    void reset_interval_timers(Pcb *p);
    interval_timer_snapshot read_interval_timer(Pcb *p, int which);
    void set_interval_timer(Pcb *p, int which, uint64 value_us, uint64 interval_us,
                            interval_timer_snapshot *old_timer);
    // 真实时间定时器必须只由全局 timekeeper CPU 扫描；每个 CPU 则独立推进
    // 当前任务的 VIRTUAL/PROF 定时器，避免 SMP 下重复投递或漏算远端线程。
    void check_interval_timers(Pcb *current_proc, bool check_realtime = true);
} // namespace proc

// 扩展的定时器结构体定义
struct extended_posix_timer
{
    int timer_id;               // 定时器 ID
    int clockid;                // 时钟类型
    proc::Pcb *owner;           // 定时器所属进程，过期时应向它投递通知
    struct sigevent
    {
        int sigev_notify;
        int sigev_signo;
        union sigval
        {
            int sival_int;
            void *sival_ptr;
        } sigev_value;
        int sigev_notify_thread_id; // SIGEV_THREAD_ID 的目标 TID，其他通知方式为 0
    } event;                    // 事件配置
    bool active;                // 是否激活
    bool armed;                 // 是否武装（设置了过期时间）
    int overrun;                // 最近一次信号投递对应的超限次数，饱和到 INT_MAX
    tmm::itimerspec spec;       // 定时器规格
    tmm::timespec expiry_time;  // 绝对过期时间
};

// 全局定时器数组的外部声明
extern extended_posix_timer g_timers[32];
extern int g_next_timer_id;
extern bool g_timers_initialized;

// POSIX timer 槽是跨进程、跨 CPU 的全局状态。所有直接访问 g_timers 的
// 路径必须持有这把锁；初始化在本地中断开启前由 syscall 子系统完成。
void init_posix_timers();
void lock_posix_timers();
void unlock_posix_timers();
bool posix_timer_owned_by_locked(const extended_posix_timer &timer, const proc::Pcb *task);
void set_posix_timer_armed_locked(extended_posix_timer &timer, bool armed);
void clear_posix_timer_locked(extended_posix_timer &timer);

// 检查全局 POSIX timer_create()/timer_settime() 定时器
void check_expired_timers();
// 进程退出时删除它创建的 POSIX timer，避免 PCB 复用后旧 timer 把信号投递给后续测例。
void cleanup_posix_timers_for_owner(proc::Pcb *owner);

#include "proc/posix_timers.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/signal.hh"
#include "printer.hh"

// 全局静态定时器数组的定义
extended_posix_timer g_timers[32];
int g_next_timer_id = 1;
bool g_timers_initialized = false;

namespace
{
constexpr uint64 k_usec_per_sec = 1000000ULL;
constexpr int k_interval_timer_real = 0;
constexpr int k_interval_timer_virtual = 1;
constexpr int k_interval_timer_prof = 2;

uint64 ticks_to_usec(uint64 ticks)
{
  uint64 cycles = ticks * tmm::cycles_per_tick();
  return tmm::time_stamp_to_usec(cycles);
}

uint64 realtime_now_usec()
{
  tmm::timeval now = tmm::k_tm.get_time_val();
  return now.tv_sec * k_usec_per_sec + now.tv_usec;
}

int timer_signal_for_kind(int which)
{
  switch (which)
  {
  case k_interval_timer_real:
    return proc::ipc::signal::SIGALRM;
  case k_interval_timer_virtual:
    return proc::ipc::signal::SIGVTALRM;
  case k_interval_timer_prof:
    return proc::ipc::signal::SIGPROF;
  default:
    return -1;
  }
}

uint64 interval_timer_now_usec(proc::Pcb *p, int which)
{
  if (p == nullptr)
  {
    return 0;
  }

  switch (which)
  {
  case k_interval_timer_real:
    return realtime_now_usec();
  case k_interval_timer_virtual:
    return ticks_to_usec(p->_user_ticks);
  case k_interval_timer_prof:
  {
    uint64 total_ticks = p->_user_ticks + p->_stime;
    proc::Pcb *current = proc::k_pm.get_cur_pcb();
    if (current == p && p->_kernel_entry_tick > 0)
    {
      uint64 now_tick = tmm::k_tm.get_ticks();
      total_ticks += now_tick - p->_kernel_entry_tick;
    }
    return ticks_to_usec(total_ticks);
  }
  default:
    return 0;
  }
}

void maybe_fire_interval_timer(proc::Pcb *p, int which, uint64 now_us)
{
  if (p == nullptr || !proc::is_valid_interval_timer_kind(which))
  {
    return;
  }

  proc::interval_timer_state &timer = p->_itimer[which];
  if (!timer.armed || now_us < timer.expiry_us)
  {
    return;
  }

  int signo = timer_signal_for_kind(which);
  if (signo > 0)
  {
    p->add_signal(signo);
  }

  if (timer.interval_us == 0)
  {
    timer.armed = false;
    timer.expiry_us = 0;
    return;
  }

  do
  {
    timer.expiry_us += timer.interval_us;
  } while (timer.expiry_us <= now_us);
}
} // namespace

namespace proc
{
bool is_valid_interval_timer_kind(int which)
{
  return which >= 0 && which < k_interval_timer_count;
}

void reset_interval_timers(Pcb *p)
{
  if (p == nullptr)
  {
    return;
  }

  for (int i = 0; i < k_interval_timer_count; ++i)
  {
    p->_itimer[i].armed = false;
    p->_itimer[i].interval_us = 0;
    p->_itimer[i].expiry_us = 0;
  }
}

interval_timer_snapshot read_interval_timer(Pcb *p, int which)
{
  interval_timer_snapshot snapshot{0, 0};
  if (p == nullptr || !is_valid_interval_timer_kind(which))
  {
    return snapshot;
  }

  uint64 now_us = interval_timer_now_usec(p, which);
  maybe_fire_interval_timer(p, which, now_us);

  interval_timer_state &timer = p->_itimer[which];
  snapshot.interval_us = timer.interval_us;
  if (timer.armed && timer.expiry_us > now_us)
  {
    snapshot.value_us = timer.expiry_us - now_us;
  }
  return snapshot;
}

void set_interval_timer(Pcb *p, int which, uint64 value_us, uint64 interval_us,
                        interval_timer_snapshot *old_timer)
{
  if (p == nullptr || !is_valid_interval_timer_kind(which))
  {
    return;
  }

  if (old_timer != nullptr)
  {
    *old_timer = read_interval_timer(p, which);
  }

  interval_timer_state &timer = p->_itimer[which];
  timer.interval_us = interval_us;
  if (value_us == 0)
  {
    timer.armed = false;
    timer.expiry_us = 0;
    return;
  }

  timer.armed = true;
  timer.expiry_us = interval_timer_now_usec(p, which) + value_us;
}

void check_interval_timers(Pcb *current_proc)
{
  uint64 real_now_us = realtime_now_usec();

  // ITIMER_REAL 基于真实时间，哪怕进程暂时没在 CPU 上跑，也应该持续倒计时。
  for (uint i = 0; i < num_process; ++i)
  {
    Pcb &candidate = k_proc_pool[i];
    if (candidate._state == ProcState::UNUSED)
    {
      continue;
    }
    maybe_fire_interval_timer(&candidate, k_interval_timer_real, real_now_us);
  }

  // CPU 时间定时器只对当前正在执行的进程推进即可，避免把别的进程的 CPU 时间偷算进去。
  if (current_proc != nullptr)
  {
    maybe_fire_interval_timer(current_proc, k_interval_timer_virtual,
                              interval_timer_now_usec(current_proc, k_interval_timer_virtual));
    maybe_fire_interval_timer(current_proc, k_interval_timer_prof,
                              interval_timer_now_usec(current_proc, k_interval_timer_prof));
  }
}
} // namespace proc

// Check for expired POSIX timers and send appropriate signals
void check_expired_timers()
{
  if (!g_timers_initialized) {
    return;  // No timers to check
  }
  
  // Get current time
  tmm::timespec current_time;
  if (tmm::k_tm.clock_gettime(tmm::SystemClockId::CLOCK_REALTIME, &current_time) != 0) {
    return;  // Can't get current time
  }
  
  // Check each timer for expiration
  for (int i = 0; i < 32; i++) {
    if (!g_timers[i].active || !g_timers[i].armed) {
      continue;  // Skip inactive or disarmed timers
    }
    
    // Check if this timer has expired
    bool expired = false;
    if (g_timers[i].expiry_time.tv_sec < current_time.tv_sec) {
      expired = true;
    } else if (g_timers[i].expiry_time.tv_sec == current_time.tv_sec && 
               g_timers[i].expiry_time.tv_nsec <= current_time.tv_nsec) {
      expired = true;
    }
    
    if (expired) {
      printfCyan("[TIMER] Timer %d expired, sending signal %d\n", 
                 g_timers[i].timer_id, g_timers[i].event.sigev_signo);
      
      // Send the signal to all processes (simplified implementation)
      // In a real implementation, you would send to the specific process that owns the timer
      proc::Pcb *current_process = proc::k_pm.get_cur_pcb();
      if (current_process) {
        // Send SIGALRM signal
        proc::ipc::signal::add_signal(current_process, g_timers[i].event.sigev_signo);
      }
      
      // Handle periodic timers
      if (g_timers[i].spec.it_interval.tv_sec > 0 || g_timers[i].spec.it_interval.tv_nsec > 0) {
        // Rearm the timer with the interval
        g_timers[i].expiry_time.tv_sec = current_time.tv_sec + g_timers[i].spec.it_interval.tv_sec;
        g_timers[i].expiry_time.tv_nsec = current_time.tv_nsec + g_timers[i].spec.it_interval.tv_nsec;
        
        // Handle nanosecond overflow
        if (g_timers[i].expiry_time.tv_nsec >= 1000000000) {
          g_timers[i].expiry_time.tv_sec++;
          g_timers[i].expiry_time.tv_nsec -= 1000000000;
        }
        
        printfCyan("[TIMER] Timer %d rearmed for next interval at %ld.%09ld\n", 
                   g_timers[i].timer_id, 
                   g_timers[i].expiry_time.tv_sec, 
                   g_timers[i].expiry_time.tv_nsec);
      } else {
        // One-shot timer: disarm it
        g_timers[i].armed = false;
        printfCyan("[TIMER] One-shot timer %d disarmed\n", g_timers[i].timer_id);
      }
    }
  }
}

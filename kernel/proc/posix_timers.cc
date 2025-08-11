#include "proc/posix_timers.hh"
#include "proc/proc_manager.hh"
#include "proc/signal.hh"
#include "printer.hh"

// 全局静态定时器数组的定义
extended_posix_timer g_timers[32];
int g_next_timer_id = 1;
bool g_timers_initialized = false;

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

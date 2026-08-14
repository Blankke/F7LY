#include "proc/posix_timers.hh"
#include "proc/capability.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "proc/signal.hh"
#include "hal/smp.hh"
#include "printer.hh"
#include "syscall_abi.hh"

// 全局静态定时器数组的定义
extended_posix_timer g_timers[32];
int g_next_timer_id = 1;
bool g_timers_initialized = false;
SpinLock g_posix_timer_lock;
eastl::atomic<uint32> g_armed_posix_timers{0};

namespace
{
constexpr int k_interval_timer_real = 0;
constexpr int k_interval_timer_virtual = 1;
constexpr int k_interval_timer_prof = 2;

// CPU0 只在确实存在 ITIMER_REAL 时扫描 PCB 池。此前每个 tick 固定读取
// 全部 1024 个 PCB，即使系统没有任何 timer，也会让零超时 epoll 偶发跨过
// 1ms 的 LTP 上限。计数只跟随 armed 的 false/true 边沿变化，PCB 中的
// atomic armed 仍是单个 timer 的权威状态。
eastl::atomic<uint32> g_armed_realtime_interval_timers{0};

void note_realtime_interval_timer_armed(int which)
{
  if (which == k_interval_timer_real)
  {
    g_armed_realtime_interval_timers.fetch_add(1, eastl::memory_order_release);
  }
}

void note_realtime_interval_timer_disarmed(int which)
{
  if (which == k_interval_timer_real)
  {
    g_armed_realtime_interval_timers.fetch_sub(1, eastl::memory_order_acq_rel);
  }
}

bool timespec_less_or_equal(const tmm::timespec &lhs, const tmm::timespec &rhs)
{
  return lhs.tv_sec < rhs.tv_sec ||
         (lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec <= rhs.tv_nsec);
}

uint64 timespec_delta_ns(const tmm::timespec &later, const tmm::timespec &earlier)
{
  if (!timespec_less_or_equal(earlier, later))
  {
    return 0;
  }

  uint64 seconds = static_cast<uint64>(later.tv_sec - earlier.tv_sec);
  long nanoseconds = later.tv_nsec - earlier.tv_nsec;
  if (nanoseconds < 0)
  {
    if (seconds == 0)
    {
      return 0;
    }
    --seconds;
    nanoseconds += 1000000000L;
  }
  const uint64 nanos = static_cast<uint64>(nanoseconds);
  if (seconds > (UINT64_MAX - nanos) / 1000000000ULL)
  {
    return UINT64_MAX;
  }
  return seconds * 1000000000ULL + nanos;
}

uint64 timespec_interval_ns(const tmm::timespec &interval)
{
  if (interval.tv_sec < 0 || interval.tv_nsec < 0)
  {
    return 0;
  }
  const uint64 seconds = static_cast<uint64>(interval.tv_sec);
  const uint64 nanos = static_cast<uint64>(interval.tv_nsec);
  if (seconds > (UINT64_MAX - nanos) / 1000000000ULL)
  {
    return UINT64_MAX;
  }
  return seconds * 1000000000ULL + nanos;
}

tmm::timespec timespec_add_ns(const tmm::timespec &base, uint64 nanoseconds)
{
  tmm::timespec result = base;
  result.tv_sec += static_cast<long>(nanoseconds / 1000000000ULL);
  result.tv_nsec += static_cast<long>(nanoseconds % 1000000000ULL);
  if (result.tv_nsec >= 1000000000L)
  {
    ++result.tv_sec;
    result.tv_nsec -= 1000000000L;
  }
  return result;
}

uint64 ticks_to_usec(uint64 ticks)
{
  uint64 cycles = ticks * tmm::cycles_per_tick();
  return tmm::time_stamp_to_usec(cycles);
}

uint64 realtime_now_usec()
{
  // ITIMER_REAL 统计的是“真实经过时间”，Linux 不会因为 clock_settime(CLOCK_REALTIME)
  // 把墙钟跳到未来就立刻触发 SIGALRM。LTP clock_settime03 会临时设置 2038 年时间，
  // 若这里使用墙钟，测试框架自己的 timeout 会抢在被测 POSIX timer 前触发。
  return tmm::time_stamp_to_usec(tmm::get_hw_time_stamp());
}

bool is_cpu_clock(tmm::SystemClockId clockid)
{
  return clockid == tmm::CLOCK_PROCESS_CPUTIME_ID ||
         clockid == tmm::CLOCK_THREAD_CPUTIME_ID;
}

bool read_task_cpu_clock(proc::Pcb *p, tmm::timespec *tp)
{
  if (p == nullptr || tp == nullptr || p->_state == proc::ProcState::UNUSED)
  {
    return false;
  }

  uint64 cpt = tmm::cycles_per_tick();
  uint64 freq = tmm::clock_frequency_hz();
  uint64 user_time_cycles = p->get_user_ticks() * cpt;
  uint64 user_time_sec = user_time_cycles / freq;
  uint64 user_time_nsec = ((user_time_cycles % freq) * 1000000000L) / freq;
  uint64 stime = p->get_stime();
  uint64 total_sec = user_time_sec + (stime / 1000000);
  uint64 total_nsec = user_time_nsec + ((stime % 1000000) * 1000);

  if (total_nsec >= 1000000000L)
  {
    total_sec += total_nsec / 1000000000L;
    total_nsec %= 1000000000L;
  }
  if (total_sec == 0 && total_nsec == 0)
  {
    total_nsec = 1;
  }

  tp->tv_sec = static_cast<long>(total_sec);
  tp->tv_nsec = static_cast<long>(total_nsec);
  return true;
}

bool read_posix_timer_clock(const extended_posix_timer &timer, tmm::timespec *current_time)
{
  tmm::SystemClockId clockid = static_cast<tmm::SystemClockId>(timer.clockid);
  if (is_cpu_clock(clockid))
  {
    // POSIX CPU timer 属于创建它的进程/线程，不能在时钟中断里偷用“当前 PCB”。
    // 中断上下文可能没有当前进程，因此异步扫描只能使用 timer 记录的 owner。
    return read_task_cpu_clock(timer.owner, current_time);
  }
  return tmm::k_tm.clock_gettime(clockid, current_time) == 0;
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
  if (!timer.armed.load(eastl::memory_order_acquire) || now_us < timer.expiry_us)
  {
    return;
  }

  int signo = timer_signal_for_kind(which);
  if (signo > 0)
  {
    p->add_signal(signo);
    // ITIMER_REAL/SIGALRM 必须能打断 accept/read/select 等普通阻塞睡眠。
    // 具体 syscall 醒来后会检查 pending signal 并返回 EINTR。这里的所有
    // 调用点均持有 p->_lock；必须先从 futex/wait-channel 索引摘链，再发布
    // RUNNABLE，不能直接改任务状态。
    if (proc::k_pm.interrupt_sleep_for_signal(p))
    {
      const int wake_cpu = p->_last_cpu;
      if (wake_cpu >= 0)
      {
        hal::smp::kick_cpu(static_cast<uint64>(wake_cpu));
      }
    }
  }

  if (timer.interval_us == 0)
  {
    note_realtime_interval_timer_disarmed(which);
    timer.armed.store(false, eastl::memory_order_release);
    timer.expiry_us = 0;
    return;
  }

  do
  {
    timer.expiry_us += timer.interval_us;
  } while (timer.expiry_us <= now_us);
}

bool should_deliver_posix_timer_signal(int notify)
{
  return notify == syscall::abi::k_sigev_signal ||
         notify == syscall::abi::k_sigev_thread_id;
}

proc::Pcb *find_posix_timer_signal_target(const extended_posix_timer &timer)
{
  proc::Pcb *owner = timer.owner;
  if (owner == nullptr || owner->_state == proc::ProcState::UNUSED)
  {
    return nullptr;
  }

  if (timer.event.sigev_notify != syscall::abi::k_sigev_thread_id)
  {
    return owner;
  }

  proc::Pcb *target =
      proc::k_capability.find_live_task_by_pid_or_tid(timer.event.sigev_notify_thread_id);
  if (target == nullptr || target->_tgid != owner->_tgid)
  {
    return nullptr;
  }
  return target;
}

void wake_if_signal_interruptible(proc::Pcb *target, int sig)
{
  if (target == nullptr)
  {
    return;
  }

  if (proc::k_pm.interrupt_sleep_for_signal(target))
  {
    const int wake_cpu = target->_last_cpu;
    if (wake_cpu >= 0)
    {
      hal::smp::kick_cpu(static_cast<uint64>(wake_cpu));
    }
    return;
  }

  if (target->_state == proc::ProcState::STOPPED &&
      (sig == proc::ipc::signal::SIGCONT || sig == proc::ipc::signal::SIGKILL))
  {
    proc::k_scheduler.set_task_state(*target, proc::ProcState::RUNNABLE);
    if (sig == proc::ipc::signal::SIGCONT)
    {
      target->_continued_pending = true;
    }
    const int wake_cpu = target->_last_cpu;
    if (wake_cpu >= 0)
    {
      hal::smp::kick_cpu(static_cast<uint64>(wake_cpu));
    }
  }
}

proc::interval_timer_snapshot read_interval_timer_locked(proc::Pcb *p, int which)
{
  proc::interval_timer_snapshot snapshot{0, 0};
  uint64 now_us = interval_timer_now_usec(p, which);
  maybe_fire_interval_timer(p, which, now_us);

  proc::interval_timer_state &timer = p->_itimer[which];
  snapshot.interval_us = timer.interval_us;
  if (timer.armed.load(eastl::memory_order_acquire) && timer.expiry_us > now_us)
  {
    snapshot.value_us = timer.expiry_us - now_us;
  }
  return snapshot;
}

void deliver_posix_timer_signal(const extended_posix_timer &timer)
{
  if (!should_deliver_posix_timer_signal(timer.event.sigev_notify))
  {
    return;
  }

  int signo = timer.event.sigev_signo;
  if (signo <= 0 || signo > proc::ipc::signal::SIGRTMAX)
  {
    return;
  }

  proc::Pcb *target = find_posix_timer_signal_target(timer);
  if (target == nullptr)
  {
    return;
  }

  target->_lock.acquire();
  if (target->_state == proc::ProcState::UNUSED ||
      target->_state == proc::ProcState::ZOMBIE ||
      target->_tgid != timer.owner->_tgid)
  {
    target->_lock.release();
    return;
  }

  proc::ipc::signal::LinuxSigInfo info{};
  info.si_signo = signo;
  info.si_errno = 0;
  info.si_code = syscall::abi::k_si_timer;
  info.si_pid = 0;
  info.si_uid = 0;
  info.si_value.sival_ptr = reinterpret_cast<uint64>(timer.event.sigev_value.sival_ptr);

  proc::ipc::signal::add_signal(target, signo, &info);
  wake_if_signal_interruptible(target, signo);
  target->_lock.release();
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
    if (p->_itimer[i].armed.load(eastl::memory_order_acquire))
    {
      note_realtime_interval_timer_disarmed(i);
    }
    p->_itimer[i].armed.store(false, eastl::memory_order_release);
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

  p->_lock.acquire();
  snapshot = read_interval_timer_locked(p, which);
  p->_lock.release();
  return snapshot;
}

void set_interval_timer(Pcb *p, int which, uint64 value_us, uint64 interval_us,
                        interval_timer_snapshot *old_timer)
{
  if (p == nullptr || !is_valid_interval_timer_kind(which))
  {
    return;
  }

  p->_lock.acquire();
  if (old_timer != nullptr)
  {
    *old_timer = read_interval_timer_locked(p, which);
  }

  interval_timer_state &timer = p->_itimer[which];
  const bool was_armed = timer.armed.load(eastl::memory_order_acquire);
  timer.interval_us = interval_us;
  if (value_us == 0)
  {
    if (was_armed)
    {
      note_realtime_interval_timer_disarmed(which);
    }
    timer.armed.store(false, eastl::memory_order_release);
    timer.expiry_us = 0;
    p->_lock.release();
    return;
  }

  timer.expiry_us = interval_timer_now_usec(p, which) + value_us;
  // expiry/interval 先就绪，再用 release 发布 armed；时钟中断
  // 的 acquire 提示因此不会看到半初始化状态。
  timer.armed.store(true, eastl::memory_order_release);
  if (!was_armed)
  {
    note_realtime_interval_timer_armed(which);
  }
  p->_lock.release();
}

void check_interval_timers(Pcb *current_proc, bool check_realtime)
{
  if (check_realtime &&
      g_armed_realtime_interval_timers.load(eastl::memory_order_acquire) != 0)
  {
    const uint64 real_now_us = realtime_now_usec();

    // ITIMER_REAL 基于真实时间，哪怕进程暂时没在 CPU 上跑，也应该持续倒计时。
    // 该全表扫描只能由 timekeeper CPU 执行，否则同一个 timer 可能并发重复触发。
    for (uint i = 0; i < num_process; ++i)
    {
      Pcb &candidate = k_proc_pool[i];
      // 大进程集合中通常只有少数任务持有实时 interval timer。先用原子位过滤，避免
      // CPU0 每个 tick 对整个 PCB 池逐项取锁。
      if (!candidate._itimer[k_interval_timer_real].armed.load(eastl::memory_order_acquire))
      {
        continue;
      }
      candidate._lock.acquire();
      if (candidate._state == ProcState::UNUSED ||
          candidate._state == ProcState::ZOMBIE)
      {
        candidate._lock.release();
        continue;
      }
      maybe_fire_interval_timer(&candidate, k_interval_timer_real, real_now_us);
      candidate._lock.release();
    }
  }

  // CPU 时间定时器只对当前正在执行的进程推进即可，避免把别的进程的 CPU 时间偷算进去。
  if (current_proc != nullptr &&
      (current_proc->_itimer[k_interval_timer_virtual].armed.load(eastl::memory_order_acquire) ||
       current_proc->_itimer[k_interval_timer_prof].armed.load(eastl::memory_order_acquire)))
  {
    current_proc->_lock.acquire();
    maybe_fire_interval_timer(current_proc, k_interval_timer_virtual,
                              interval_timer_now_usec(current_proc, k_interval_timer_virtual));
    maybe_fire_interval_timer(current_proc, k_interval_timer_prof,
                              interval_timer_now_usec(current_proc, k_interval_timer_prof));
    current_proc->_lock.release();
  }
}
} // namespace proc

void clear_posix_timer_locked(extended_posix_timer &timer)
{
  if (timer.armed)
  {
    g_armed_posix_timers.fetch_sub(1, eastl::memory_order_acq_rel);
  }
  timer = {};
}

void set_posix_timer_armed_locked(extended_posix_timer &timer, bool armed)
{
  if (timer.armed == armed)
  {
    return;
  }
  timer.armed = armed;
  if (armed)
  {
    g_armed_posix_timers.fetch_add(1, eastl::memory_order_release);
  }
  else
  {
    g_armed_posix_timers.fetch_sub(1, eastl::memory_order_acq_rel);
  }
}

bool posix_timer_owned_by_locked(const extended_posix_timer &timer, const proc::Pcb *task)
{
  return timer.active && timer.owner != nullptr && task != nullptr &&
         timer.owner->_tgid == task->_tgid;
}

void init_posix_timers()
{
  g_posix_timer_lock.init("posix_timer");
  g_armed_posix_timers.store(0, eastl::memory_order_relaxed);
  for (auto &timer : g_timers)
  {
    clear_posix_timer_locked(timer);
  }
  g_next_timer_id = 1;
  g_timers_initialized = true;
}

void lock_posix_timers()
{
  g_posix_timer_lock.acquire();
}

void unlock_posix_timers()
{
  g_posix_timer_lock.release();
}

// Check for expired POSIX timers and send appropriate signals
void check_expired_timers()
{
  if (!g_timers_initialized) {
    return;  // No timers to check
  }
  if (g_armed_posix_timers.load(eastl::memory_order_acquire) == 0) {
    return;
  }

  lock_posix_timers();
  // Check each timer for expiration
  for (int i = 0; i < 32; i++) {
    if (!g_timers[i].active || !g_timers[i].armed) {
      continue;  // Skip inactive or disarmed timers
    }

    tmm::timespec current_time;
    if (!read_posix_timer_clock(g_timers[i], &current_time)) {
      continue;  // 当前时钟不可读时不要误触发别的时钟域定时器。
    }
    
    if (timespec_less_or_equal(g_timers[i].expiry_time, current_time)) {
      // Handle periodic timers
      if (g_timers[i].spec.it_interval.tv_sec > 0 || g_timers[i].spec.it_interval.tv_nsec > 0) {
        // 用除法一次算出 missed expiration。旧实现按 interval 逐次相加，
        // 1ns 周期且绝对过期时间落后数秒时会在中断上下文循环数十亿次。
        const uint64 interval_ns = timespec_interval_ns(g_timers[i].spec.it_interval);
        const uint64 elapsed_ns = timespec_delta_ns(current_time, g_timers[i].expiry_time);
        const uint64 missed = interval_ns == 0 ? 0 : elapsed_ns / interval_ns;
        g_timers[i].overrun = missed > static_cast<uint64>(INT_MAX)
                                  ? INT_MAX
                                  : static_cast<int>(missed);
        const uint64 until_next = interval_ns == 0
                                      ? 1
                                      : interval_ns - (elapsed_ns % interval_ns);
        g_timers[i].expiry_time = timespec_add_ns(current_time, until_next);
      } else {
        // One-shot timer: disarm it
        g_timers[i].overrun = 0;
        set_posix_timer_armed_locked(g_timers[i], false);
      }

      // POSIX timer 通知不能固定投给当前进程：SIGEV_SIGNAL 投给 owner，
      // SIGEV_THREAD_ID 投给用户态指定的线程，SIGEV_NONE 则只更新定时器状态。
      // overrun 必须先发布，信号处理器里的 timer_getoverrun() 才能读到本次值。
      deliver_posix_timer_signal(g_timers[i]);
    }
  }
  unlock_posix_timers();
}

void cleanup_posix_timers_for_owner(proc::Pcb *owner)
{
  if (!g_timers_initialized || owner == nullptr) {
    return;
  }

  lock_posix_timers();
  for (int i = 0; i < 32; i++) {
    if (!g_timers[i].active || g_timers[i].owner != owner) {
      continue;
    }

    // Linux 在进程退出时会删除该进程拥有的 POSIX timer。这里必须同时清掉 owner，
    // 否则全局 timer 槽里保存的 PCB 指针可能在后续测例复用后误投递信号。
    clear_posix_timer_locked(g_timers[i]);
  }
  unlock_posix_timers();
}

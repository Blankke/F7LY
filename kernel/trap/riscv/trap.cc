#ifdef RISCV
#include "types.hh"
#include "trap.hh"
#include "platform.hh"
#include "hal/riscv/sbi.hh"
#include "param.h"
#include "plic.hh"
#include "mem/memlayout.hh"
#include "devs/console.hh"
#include "printer.hh"
#include "rv_csr.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "proc/signal.hh"
#include "trap_func_wrapper.hh"
#include "syscall_handler.hh"
#include "proc.hh"
#include "mem.hh"
#include "physical_memory_manager.hh"
#include "fs/vfs/file/normal_file.hh"
#include "virtual_memory_manager.hh"
#include "timer_interface.hh"
#include "timer_manager.hh"
#include "fs/drivers/virtio_blk.hh"
#include "net/drivers/virtio_net.hh"
#include "trap/interrupt_stats.hh"
#include "proc/posix_timers.hh"

// #include "fuckyou.hh"
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern char trampoline[], uservec[], userret[];

namespace
{
  // 单核调度使用固定 tick 时间片，确保长时间运行的用户/内核态任务都能被周期性抢占。
  // CPU 密集型 rustc worker 通常能独占自己的 home CPU。40ms 时间片把无意义
  // 的 timer 强制切换降到原来的 1/4，同时仍保留足够的交互和等待唤醒响应。
  constexpr int k_default_time_slice_ticks = 4;
  constexpr uint32 k_riscv_user_asid_count = 1U << 10;
  static_assert(proc::num_process < k_riscv_user_asid_count,
                "RISC-V 用户 ASID 数量必须覆盖全部 PCB 槽位");

  inline uint32 riscv_user_asid(const proc::Pcb *p)
  {
    const uint32 asid = p->_user_asid;
    if (asid == 0 || asid >= k_riscv_user_asid_count)
    {
      panic("invalid RISC-V user ASID pid=%d tid=%d asid=%u",
            p->_pid, p->_tid, asid);
    }
    return asid;
  }
}

// 创建一个静态对象
trap_manager trap_mgr;

// 前置声明，内部函数只有这里使用。
int mmap_handler(uint64 va, int cause);

// 初始化锁
void trap_manager::init()
{
  ticks = 0;
  tickslock.init("tickslock");
  printfGreen("[trap] Trap Manager Init\n");
}

// 架构相关, 设置csr
void trap_manager::inithart()
{
  w_stvec((uint64)kernelvec);
  w_sstatus(r_sstatus() | SSTATUS_SIE);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
  set_next_timeout();
  printfGreen("[trap] Trap Manager Inithart\n");
}

// 时钟到期后, 重新设置下次超时
void trap_manager::set_next_timeout()
{
  // RISC-V 的 OpenSBI/ACLINT timer 频率是固定硬件计数器。
  // 这里统一使用时间子系统给出的每 tick 周期数，避免再维护一套
  // 与 LoongArch 不一致的独立 INTERVAL 常量。
  sbi_set_timer(r_time() + tmm::cycles_per_tick());
}

// 处理外部中断和软件中断
int trap_manager::devintr()
{
  uint64 scause = r_scause();

  if (scause == 0x8000000000000001L)
  {
    // 调度唤醒 IPI：清除 SSIP 后返回 scheduler；若本核正在运行用户任务，
    // 只把它当作一次无害中断，不额外抢占当前时间片。
    sbi_clear_ipi();
    return 3;
  }

  if ((scause & 0x8000000000000000L) &&
      (scause & 0xff) == 9)
  {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_mgr.claim();

    // intr_stats::k_intr_stats.record_interrupt(irq);
    if (irq == UART0_IRQ)
    {
      while (true)
      {
        int c = sbi_console_getchar();
        if (c < 0)
        {
          break;
        }
        dev::kConsole.console_intr(c);
      }
    }
    //!!写完磁盘后修改
    else if (irq == VIRTIO0_IRQ)
    {
      virtio_disk_intr();
    }
    else if (net::virtio_net_uses_irq(irq))
    {
      net::virtio_net_intr();
    }
    else if (irq == VIRTIO1_IRQ)
    {
      virtio_disk_intr2();
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if (irq)
      plic_mgr.complete(irq);

    return 1;
  }
  if (scause == 0x8000000000000005L)
  {
    // printfBlue("zzZ");
    // TODO, 这个5是瞎写的
    // 假设5是时钟中断
    // intr_stats::k_intr_stats.record_interrupt(5);
    // 每个 hart 都必须重设自己的 one-shot timer；全局 tick、sleep 唤醒和
    // POSIX 实时时钟只由 CPU0 推进，避免 SMP 下系统时间按核数加速。
    set_next_timeout();
    const bool is_timekeeper = Cpu::is_bootstrap_cpu();
    if (is_timekeeper)
    {
      timertick();
    }
    proc::check_interval_timers(Cpu::get_cpu()->get_cur_proc(), is_timekeeper);

    /// TODO: riscv可以用sbi的tick来实现时钟
    /// 但是loongarch只能使用tmm，在所有用了tick、timeslice的地方都要改

    // tmm::k_tm.tick_increase();

    return 2;
  }
  else
  {
    return 0;
  }
}

void trap_manager::timertick()
{
  // tickslock 只保护全局 tick 计数；唤醒和定时器扫描在锁外完成，避免时钟中断
  // 与其它 CPU 的进程锁路径形成长时间嵌套。
  tickslock.acquire();
  ticks++;
  const uint current_ticks = ticks;
  tickslock.release();

  constexpr uint k_load_sample_ticks = 5 * 1000 / tmm::ms_per_tick;
  static_assert(k_load_sample_ticks > 0);
  if ((current_ticks % k_load_sample_ticks) == 0)
  {
    proc::k_scheduler.sample_load_averages(
        static_cast<uint64>(current_ticks) * tmm::ms_per_tick / 1000);
  }

  proc::k_pm.wakeup(&ticks);
  check_expired_timers();
}

// 处理内核态的中断
// 支持嵌套中断
void trap_manager::kerneltrap()
{
  //   printfMagenta("into kerneltrap\n");
  int which_dev = 0;

  // 这些寄存器可能在yield时被修改
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  // Debug
  // printfYellow("kerneltrap: sepc=%p sstatus=%p scause=%p\n", sepc, sstatus, scause);

  // 检查中断是否来自内核态
  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  // 中断是否被屏蔽
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    proc::Pcb *cur = Cpu::get_cpu()->get_cur_proc();
    if (cur != nullptr)
    {
      panic("kerneltrap: scause=%p sepc=%p stval=%p sstatus=%p proc=%s pid=%d tid=%d state=%d mm=%p pt=%p",
            scause,
            r_sepc(),
            r_stval(),
            sstatus,
            cur->_name,
            cur->_pid,
            cur->_tid,
            cur->_state,
            cur->get_memory_manager(),
            cur->get_pagetable());
    }
    panic("kerneltrap: scause=%p sepc=%p stval=%p sstatus=%p no-current-proc",
          scause, r_sepc(), r_stval(), sstatus);
  }

  if (which_dev == 2 && Cpu::get_cpu()->get_cur_proc() != nullptr &&
      Cpu::get_cpu()->get_cur_proc()->_state == proc::RUNNING &&
      !Cpu::get_cpu()->get_cur_proc()->_exiting)
  {
    if (Cpu::get_cpu()->advance_time_slice(k_default_time_slice_ticks))
    {
      proc::k_scheduler.yield();
      // print_fuckyou();
    }
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void trap_manager::usertrap()
{
  // printfMagenta("into usertrap\n");
  int which_dev = 0;
  if ((r_sstatus() & riscv::csr::sstatus_spp_m) != 0)
    panic("usertrap: not from user mode");

  if (intr_get() != 0)
    panic("usertrap: interrupts enabled");
  // 用户态不能持有内核锁；进入新的 trap/syscall 时重置软件中断嵌套计数，
  // 防止上一轮调度残留的 noff 污染 sleep()/sched() 不变量。
  Cpu::get_cpu()->reset_intr_off_depth();
  w_stvec((uint64)kernelvec);

  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  p->_trapframe->epc = r_sepc();
  uint64 cause = r_scause();

  // 时间统计：从用户态切换到内核态
  uint64 cur_tick = tmm::get_ticks();
  if (p->_last_user_tick > 0)
  {
    // 累加用户态运行时间
    uint64 user_time = cur_tick - p->_last_user_tick;
    p->_user_ticks += user_time;
  }
  // 记录进入内核态的时间点
  p->_kernel_entry_tick = cur_tick;

  if (cause == 8)
  {
    if (proc::ipc::signal::has_signal_pending(p, proc::ipc::signal::SIGKILL))
    {
      // SIGKILL 不应再让目标任务继续执行下一次系统调用；这能打断
      // 大量文件操作/压力任务在 kill 后继续消耗时间的情况。
      proc::k_pm.do_signal_exit(p, proc::ipc::signal::SIGKILL);
    }
    if (p->is_killed())
      proc::k_pm.exit(-1);
    // printfYellow("p->_trapframe->epc: %p\n", p->_trapframe->epc);
    p->_trapframe->epc += 4;
    // printfYellow("p->_trapframe->epc: %p\n", p->_trapframe->epc);
    intr_on();
    syscall::k_syscall_handler.invoke_syscaller();
  }
  else if ((which_dev = devintr()) != 0)
  {
    // ok
  }
  else if (cause == 13 || cause == 15 || cause == 12)
  {
    // 缺页故障处理
    TODO("pagefault_handler");
    ///@brief 此处处理mmap的缺页异常
    // printfRed("p->_trapframe->sp: %p,printf fault_va:%p, p->_sz:%p\n", PGROUNDUP(p->_trapframe->sp) - 1, fault_va, p->_sz);
    if (mmap_handler(r_stval(), cause) != 0)
    {
      // 缺页异常处理失败，发送SIGSEGV信号
      printfRed("usertrap(): page fault at %p, sending SIGSEGV to pid=%d\n", r_stval(), p->_pid);
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGSEGV);
      printfRed("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->_pid);
      printfRed("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    }
  }
  else
  {
    // 处理其他未处理的异常，根据异常原因发送相应的同步信号
    uint64 scause = r_scause();
    printfRed("usertrap(): unexpected scause %p pid=%d\n", scause, p->_pid);
    printfRed("            sepc=%p stval=%p\n", r_sepc(), r_stval());

    // 根据RISC-V异常原因发送相应的同步信号
    switch (scause)
    {
    case 0: // Instruction address misaligned
    case 1: // Instruction access fault
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
      proc::ipc::signal::handle_sync_signal();
      break;
    case 2: // Illegal instruction
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGILL);
      proc::ipc::signal::handle_sync_signal();
      break;
    case 3: // Breakpoint
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGTRAP);
      proc::ipc::signal::handle_sync_signal();
      break;
    case 4: // Load address misaligned
    case 6: // Store/AMO address misaligned
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
      proc::ipc::signal::handle_sync_signal();
      break;
    case 5: // Load access fault
    case 7: // Store/AMO access fault
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGSEGV);
      proc::ipc::signal::handle_sync_signal();
      break;
    default:
      // 对于未知的异常，发送SIGSYS信号
      proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGSYS);
      proc::ipc::signal::handle_sync_signal();
      break;
    }
  }

  if (which_dev == 2 && p->_last_user_tick > 0 && cur_tick == p->_last_user_tick)
  {
    // 用户态时钟中断是在 devintr()/timertick() 里才把 ticks 加一。
    // 如果按进入 usertrap() 时的旧 ticks 记账，这一整个时间片会被吃掉，
    // ITIMER_VIRTUAL/ITIMER_PROF 与 times()/getrusage() 都会长期偏小甚至不前进。
    p->_user_ticks += 1;
    p->_kernel_entry_tick = tmm::get_ticks();
  }

  if (proc::ipc::signal::has_signal_pending(p, proc::ipc::signal::SIGKILL))
    proc::k_pm.do_signal_exit(p, proc::ipc::signal::SIGKILL);
  if (p->is_killed())
    proc::k_pm.exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && !p->_exiting)
  {
    if (Cpu::get_cpu()->advance_time_slice(k_default_time_slice_ticks))
    {
      // proc::ipc::signal::handle_signal();
      // printf("yield in usertrap\n");
      proc::k_scheduler.yield();
    }
  }
  // printfMagenta("left usertrap\n");
  proc::ipc::signal::handle_signal();
  usertrapret();
}

void trap_manager::usertrapret()
{

  // printfMagenta("into usertrapret\n");
  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  // 优先处理同步信号(紧急信号) - 在返回用户态之前检查并处理
  proc::ipc::signal::handle_sync_signal();

  // 时间统计：从内核态切换到用户态
  uint64 cur_tick = tmm::get_ticks();
  if (p->_kernel_entry_tick > 0)
  {
    // 累加内核态运行时间
    uint64 kernel_time = cur_tick - p->_kernel_entry_tick;
    p->_stime += kernel_time;
  }
  // 记录进入用户态的时间点
  p->_last_user_tick = cur_tick;

  // Debug
  //  printfYellow("[usertrapret] trampoline addr %p\n", trampoline);

  if (p == nullptr || p->get_memory_manager() == nullptr ||
      p->get_pagetable() == nullptr || !p->get_pagetable()->get_base())
  {
    panic("usertrapret: invalid current address space pid=%d tid=%d state=%d exiting=%d",
          p ? p->_pid : -1,
          p ? p->_tid : -1,
          p ? (int)p->_state : -1,
          p ? (int)p->_exiting : -1);
  }
  if (!p->get_memory_manager()->ensure_special_mappings())
  {
    panic("usertrapret: failed to ensure special mappings pid=%d tid=%d state=%d pt=%p",
          p->_pid,
          p->_tid,
          (int)p->_state,
          (void *)p->get_pagetable()->get_base());
  }

  // CLONE_VM 线程共享用户页表，但每个 PCB 有自己的 trapframe 物理页。
  // 固定复用 TRAPFRAME 会让两个核同时返回用户态时互相拆掉对方的映射。
  // 因此按 PCB 槽位分配独立的高地址页，并把实际地址传给 trampoline。
  intr_off();
  const uint64 user_trapframe_va = USER_TRAPFRAME(p->get_global_id());

  // 同一线程的 trapframe PTE 在生命周期内保持不变；只有 PCB 槽位复用时
  // 才需要替换。首次多个 CLONE_VM 线程同时返回时，页表层级创建必须串行化，
  // 否则两个 CPU 可能互相覆盖同一个父级 PTE。
  mem::PageTable *user_pt = p->get_pagetable();
  const uint64 expected_trapframe_pa = PGROUNDDOWN(
      riscv::virt_to_phy_address(reinterpret_cast<uint64>(p->get_trapframe())));
  mem::Pte trapframe_pte = user_pt->walk(user_trapframe_va, false);
  bool trapframe_mapping_matches =
      !trapframe_pte.is_null() && trapframe_pte.is_valid() &&
      reinterpret_cast<uint64>(trapframe_pte.pa()) == expected_trapframe_pa;

  if (!trapframe_mapping_matches)
  {
    mem::k_vmm.lock_page_table_updates();
    trapframe_pte = user_pt->walk(user_trapframe_va, false);
    trapframe_mapping_matches =
        !trapframe_pte.is_null() && trapframe_pte.is_valid() &&
        reinterpret_cast<uint64>(trapframe_pte.pa()) == expected_trapframe_pa;
    bool mapped = true;
    if (!trapframe_mapping_matches)
    {
      mem::k_vmm.vmunmap(*user_pt, user_trapframe_va, 1, 0);
      mapped = mem::k_vmm.map_pages(*user_pt,
                                    user_trapframe_va,
                                    PGSIZE,
                                    (uint64)p->get_trapframe(),
                                    riscv::PteEnum::pte_readable_m |
                                        riscv::PteEnum::pte_writable_m);
    }
    mem::k_vmm.unlock_page_table_updates();
    if (!mapped)
    {
      panic("usertrapret: failed to map trapframe");
    }
  }
  mem::Pte pte = p->get_pagetable()->walk(TRAMPOLINE, 0);
  if (pte.is_null() || pte.is_valid() == 0)
  {
    proc::ProcessMemoryManager *mm = p->get_memory_manager();
    mem::PageTable *pt = p->get_pagetable();
    uint64 pt_base = (pt != nullptr) ? pt->get_base() : 0;
    panic("trampoline not mapped in user pagetable! pid=%d tid=%d state=%d signal=%d mm=%p pt=%p pt_base=%p trapframe=%p epc=%p sp=%p",
          p ? p->_pid : -1,
          p ? p->_tid : -1,
          p ? (int)p->_state : -1,
          p ? p->_signal : -1,
          mm,
          pt,
          (void *)pt_base,
          p ? p->get_trapframe() : nullptr,
          p ? (void *)p->_trapframe->epc : nullptr,
          p ? (void *)p->_trapframe->sp : nullptr);
  }

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), interrupts are already off until
  // we're back in user space, where usertrap() is correct.
  w_stvec(TRAMPOLINE + (uservec - trampoline));
  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->_trapframe->kernel_satp = r_satp();
  p->get_trapframe()->kernel_sp = p->get_kstack() + KSTACK_SIZE;
  p->_trapframe->kernel_trap = (uint64)wrap_usertrap;
  p->_trapframe->kernel_hartid = r_tp();

  uint64 x = r_sstatus();
  x &= ~riscv::csr::sstatus_spp_m;
  x |= riscv::csr::sstatus_spie_m;
  // 用户和内核都会使用 FPU；保持 FS=Dirty，下一次 uservec 先保存用户现场，
  // 返回前再恢复，隔离不同任务以及内核自身的浮点寄存器使用。
  x |= 0x3ULL << 13;
  w_sstatus(x);
  w_sepc(p->_trapframe->epc);

  // printfYellow("[usertrapret] sepc: %p,saved sepc:%p\n", p->_trapframe->epc,r_sepc());
  // tell trampoline.S the user page table to switch to.

  // debug
  // printfYellow("[usertrapret]user pagetable addr: %p\n", p->_pt.get_base());

  // ASID 0 保留给内核；用户地址空间在回收池完成全核 flush 前不会复用
  // 非零 ASID，因此 trap 热路径只需切换 satp，无需每次清空本 hart 全 TLB。
  uint64 satp = MAKE_SATP_ASID(p->get_pagetable()->get_base(), riscv_user_asid(p));
  // debug

  uint64 fn = TRAMPOLINE + (userret - trampoline);
  // printf("trapframe addr: %p\n", p->_trapframe);
  //   printf("trapframe->epc: %p\n", p->_trapframe->epc);
  //   printf("[usertrapret] trapframe->a0: %p\n", p->_trapframe->a0);

  ((void (*)(uint64, uint64))fn)(user_trapframe_va, satp);
}

/**
 * @brief mmap_handler 处理mmap惰性分配导致的页面错误
 * @param va 页面故障虚拟地址
 * @param cause 页面故障原因 (13=load fault, 15=store fault)
 * @return 0成功，-1失败
 */
int mmap_handler(uint64 va, int cause)
{
  proc::Pcb *p = proc::k_pm.get_cur_pcb();
  proc::ProcessMemoryManager *mm = p != nullptr ? p->get_memory_manager() : nullptr;

  // 确定访问类型
  int access_type = 0; // 默认读取
  if (cause == 15)
  { // Store page fault
    access_type = 1; // 写入
  }
  else if (cause == 12)
  {                  // Instruction page fault
    access_type = 2; // 执行
  }

  if (mm == nullptr)
  {
    return -1;
  }
  // 用户缺页与同一 CLONE_VM 地址空间中的 mmap/munmap/mprotect 串行，
  // 保证 VMA 查找和 PTE 安装基于同一代元数据。
  mm->lock_memory();
  int result = mm->fault_page(va, access_type);
  mm->unlock_memory();
  return result;
}
#endif

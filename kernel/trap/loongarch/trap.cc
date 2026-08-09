#ifdef LOONGARCH
#include "types.hh"
#include "trap.hh"
#include "platform.hh"
#include "param.h"
// #include "plic.hh"
#include "mem.hh"
#include "mem/memlayout.hh"
#include "devs/console.hh"
#include "printer.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "trap_func_wrapper.hh"
#include "extioi.hh"
#include "trap/loongarch/pci.h"
#include "apic.hh"
#include "syscall_handler.hh"
#include "cpu.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"
#include "vfs/file/normal_file.hh"
#include "devs/loongarch/disk_driver.hh"
#include "trap/interrupt_stats.hh"
#include "timer_interface.hh"
#include "proc/posix_timers.hh"
#include "proc/futex.hh"
#include "asm.hh"
#include "hal/tlb_shootdown.hh"
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern "C" void uservec();
extern "C" void handle_tlbr();
extern "C" void handle_merr();
// userret 在切到用户 PGDL 后仍处于内核特权级。第四个参数保留当前 PCB
// trapframe 的内核直映地址，第五个参数传入当前任务分配到的用户 ASID。
extern "C" void userret(uint64, uint64, uint64, uint64, uint64);
int mmap_handler(uint64 va, int cause);
// 创建一个静态对象
trap_manager trap_mgr;

namespace
{
  // 单核调度使用固定 tick 时间片，确保长时间运行的用户/内核态任务都能被周期性抢占。
  // 与 RISC-V 保持同一调度语义；减少 rustc CPU 密集阶段的强制切换成本。
  constexpr int k_default_time_slice_ticks = 4;
  constexpr uint32 k_loongarch_ecode_fpu_disabled = 0xf;
  constexpr uint32 k_loongarch_ecode_lsx_disabled = 0x10;
  constexpr uint32 k_loongarch_asid_bits = 10;
  constexpr uint32 k_loongarch_kernel_asid = 0;
  static_assert(proc::num_process < (1U << k_loongarch_asid_bits),
                "LoongArch 用户 ASID 数量必须覆盖全部 PCB 槽位");

  inline uint32 loongarch_user_asid(const proc::ProcessMemoryManager *mm)
  {
    const uint32 asid = mm != nullptr ? mm->user_asid : 0;
    if (asid == k_loongarch_kernel_asid || asid >= (1U << k_loongarch_asid_bits))
    {
      panic("invalid LoongArch user ASID mm=%p asid=%u", mm, asid);
    }
    return asid;
  }

  // LoongArch 异常信息拆分：一级编码在 ESTAT[21:16]，二级编码在 ESTAT[30:22]。
  // 之前把 ecode=8 直接当成缺页，会把 ADEM（访存地址错误）误送进 mmap 懒分配路径。
  inline uint32 loongarch_exception_code(uint32 estat)
  {
    return (estat & CSR_ESTAT_ECODE) >> 16;
  }

  inline uint32 loongarch_exception_subcode(uint32 estat)
  {
    return (estat >> 22) & 0x1ffU;
  }

  inline bool is_loongarch_page_fault_code(uint32 ecode)
  {
    return ecode >= 0x1 && ecode <= 0x7;
  }

  inline void loongarch_invalidate_user_tlb_page(uint32 asid, uint64 va)
  {
    // LoongArch 一个普通 TLB 表项覆盖相邻两页，Linux 也会按 8KB 对齐做单页失效。
    // 这里对“PTE 已合法存在、只是 TLB 内部残着无效项”的场景做最小失效，
    // 避免把它误判成 mmap 懒分配失败后直接送 SIGSEGV。
    uint64 pair_base = va & ~((PGSIZE << 1) - 1);
    asm volatile("invtlb 0x6, %0, %1" : : "r"(static_cast<uint64>(asid)), "r"(pair_base) : "memory");
  }

  inline void loongarch_ack_timer_interrupt()
  {
    // TICLR/TINTCLR 是“写 1 清中断”的专用寄存器，按 Linux 的做法直接写清除位即可。
    // 这里不要先读再 OR 回写：
    // 1. 该寄存器本身没有需要保留的状态位；
    // 2. 读值可能是未定义/瞬时值，反而容易把 timer pending 留到 userret 窗口里，
    //    让同一个时钟中断在刚返回用户态时又立刻打回来。
    w_csr_ticlr(CSR_TICLR_CLR);
  }

  inline bool loongarch_can_retry_present_user_fault(mem::Pte pte, uint32 ecode)
  {
    if (pte.is_null() || !pte.is_valid() || !pte.is_present() || pte.is_super_plv())
    {
      return false;
    }

    switch (ecode)
    {
    case 0x1: // load invalid
      return pte.is_readable();
    case 0x2: // store invalid
      return pte.is_writable();
    case 0x3: // fetch invalid
      return pte.is_executable();
    case 0x4: // modified fault
      return pte.is_writable();
    default:
      return false;
    }
  }

}

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
  uint32 ecfg = (0U << CSR_ECFG_VS_SHIFT) | HWI_VEC | TI_VEC | IPI_VEC;
  // LoongArch 的 timer CSR 直接按周期数编程。这里必须与 tmm::cycles_per_tick()
  // 保持一致，否则 sleep()/CPU 计时/interval timer 会共同漂移，
  // 用户可见的定时器精度会被放大到错误量级。
  uint64 tcfg = tmm::cycles_per_tick() | CSR_TCFG_EN | CSR_TCFG_PER;

  w_csr_ecfg(ecfg);
  w_csr_tcfg(tcfg);

  w_csr_eentry((uint64)kernelvec);
  w_csr_tlbrentry((uint64)handle_tlbr);
  w_csr_merrentry((uint64)handle_merr);
  intr_on();
}

// 处理外部中断和软件中断
int trap_manager::devintr()
{
  static bool pcie_irq_warned = false;

  uint32 estat = r_csr_estat();
  uint32 ecfg = r_csr_ecfg();
  uint32 ecode = loongarch_exception_code(estat);

  // LoongArch 的 ESTAT 同时混合了“异常编码”和“中断待处理位”。
  // 如果只盯 pending bit，不检查 ecode，内核在处理别的同步异常时，
  // 只要此刻恰好挂着一个 timer pending，就会被误判成时钟中断，
  // 然后重入 timertick()，最终在 tickslock 上表现成同核递归拿锁。
  // 这里先确保当前 trap 的一级编码确实是“中断”场景，再进入中断分发。
  if (ecode != 0)
  {
    return 0;
  }

  if (estat & ecfg & IPI_VEC)
  {
    if (hal::tlb::handle_ipi())
    {
      return 3;
    }
  }

  if (estat & ecfg & HWI_VEC)
  {
    // this is a hardware interrupt, via IOCR.

    // irq indicates which device interrupted.
    uint64 irq = extioi_claim();
    // printf("%d\n", irq);
    // 处理串口中断
    uint64 handled_irq_mask = 0;
    if (irq & (1UL << UART0_IRQ))
    {
      // 交互式 shell/stdin 依赖串口 RX 中断把字符推进 console 行规程；
      // 这里只做 ack 会导致输出正常、输入永远到不了 kConsole。
      dev::k_uart.handle_intr();
      handled_irq_mask |= (1UL << UART0_IRQ);
      apic_complete(1UL << UART0_IRQ);
      extioi_complete(1UL << UART0_IRQ);
    }

    if (irq & (1UL << PCIE_IRQ))
    {
      // TODO
      // intr_stats::k_intr_stats.record_interrupt(PCIE_IRQ);
      // loongarch::qemu::disk_driver.handle_intr();
            if (!pcie_irq_warned)
      {
        pcie_irq_warned = true;
        printfYellow("[trap] PCIE 中断当前未走内核分发路径，先确认并放行\n");
      }
      handled_irq_mask |= (1UL << PCIE_IRQ);
      apic_complete(1UL << PCIE_IRQ);
      extioi_complete(1UL << PCIE_IRQ);
      printfYellow("未实现PCIE_IRQ中断处理,不过好像跟riscv不一样，跟蒙老师也不一样，现在好像不用这个\n");
    }

    uint64 remaining_irq = irq & ~handled_irq_mask;
    if (remaining_irq)
    {
      printf("unexpected interrupt irq=%d\n", remaining_irq);

      apic_complete(remaining_irq);
      extioi_complete(remaining_irq);
    }

    return 1;
  }
  else if (estat & ecfg & TI_VEC)
  {
    // timer interrupt.
    // 先清 pending，再做较重的 tick/唤醒/定时器检查，避免中途再进一次 trap
    // 时又把这同一个 pending timer 误当成“新的时钟中断”。
    loongarch_ack_timer_interrupt();

    const bool is_timekeeper = Cpu::is_bootstrap_cpu();
    if (is_timekeeper)
    {
      timertick();
    }
    proc::check_interval_timers(Cpu::get_cpu()->get_cur_proc(), is_timekeeper);

    return 2;
  }
  else
  {
    return 0;
  }
}

void trap_manager::timertick()
{
  // tickslock 只保护全局 tick 计数本身，复杂逻辑尽量放到锁外，
  // 降低中断路径里持锁时间，也减少后续异常把现场糊成“递归拿 tickslock”的概率。
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
  proc::futex_check_timeouts(tmm::get_hw_time_stamp());

  // POSIX realtime timer 的全局扫描只由 CPU0 执行，其他 CPU 在 devintr() 中
  // 单独推进自己的 VIRTUAL/PROF timer。
  check_expired_timers();
}

// !!写完进程后修改
void trap_manager::usertrap()
{
  // printfMagenta("==usertrap==\n");

  int which_dev = 0;

  if ((r_csr_prmd() & PRMD_PPLV) == 0)
    panic("usertrap: not from user mode");

  // 用户态不会合法持有内核自旋锁；每次从用户态进入内核时归一化
  // 软件中断嵌套计数，避免跨调度残留影响 wait/sleep/sched 路径。
  Cpu::get_cpu()->reset_intr_off_depth();

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_csr_eentry((uint64)kernelvec);

  proc::Pcb *p = proc::k_pm.get_cur_pcb();
  // uservec 通过当前线程的用户 trapframe 恢复内核栈。CLONE_VM 多线程若
  // 错把别的槽位的 trapframe 带进来，最早且最可靠的信号就是当前 SP 不再
  // 落在 CPU 所记录 PCB 的内核栈内；立刻终止而不是继续用错现场执行系统调用。
  const uint64 current_kernel_sp = Cpu::read_sp();
  const uint64 expected_kstack_bottom = p->get_kstack();
  const uint64 expected_kstack_top = expected_kstack_bottom + KSTACK_SIZE;
  if (current_kernel_sp < expected_kstack_bottom || current_kernel_sp > expected_kstack_top)
  {
    panic("usertrap: trapframe/kernel stack mismatch cpu=%lu pid=%d tid=%d gid=%d sp=%p expected=[%p,%p)",
          Cpu::current_cpu_id(),
          p->_pid,
          p->_tid,
          p->_global_id,
          (void *)current_kernel_sp,
          (void *)expected_kstack_bottom,
          (void *)expected_kstack_top);
  }
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

  // save user program counter.
  p->_trapframe->era = r_csr_era();

  uint32 estat = r_csr_estat();
  uint32 ecode = loongarch_exception_code(estat);
  uint32 esubcode = loongarch_exception_subcode(estat);

  if (ecode == 0xb)
  {
    // system call

    if (proc::ipc::signal::has_signal_pending(p, proc::ipc::signal::SIGKILL))
    {
      // SIGKILL 不应再让目标任务继续执行下一次系统调用；这能打断
      // 大量文件操作/压力任务在 kill 后继续消耗时间的情况。
      proc::k_pm.do_signal_exit(p, proc::ipc::signal::SIGKILL);
    }
    if (p->_killed)
      proc::k_pm.exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->_trapframe->era += 4;

    // an interrupt will change crmd & prmd registers,
    // so don't enable until done with those registers.
    intr_on();
    syscall::k_syscall_handler.invoke_syscaller();
    intr_off();
  }
  else if (ecode == k_loongarch_ecode_fpu_disabled)
  {
    // 懒 FPU：整数/内存类程序不应该在每次 syscall/page fault 都保存 32 个 FPR。
    // 用户第一次执行浮点指令时硬件打 FPD 异常；这里只标记当前线程需要 FPU，
    // 不推进 era，让 userret 恢复该线程保存的 FPU 现场后重试原指令。
    p->_used_fpu = true;
  }
  else if (ecode == k_loongarch_ecode_lsx_disabled)
  {
    /*
     * LSX 与标量 FPR 共享每个向量寄存器的低 64 位。线程第一次触发
     * SXD 时，uservec 已经保存了标量 FPR；把它们迁入完整向量镜像，
     * 高 64 位按体系结构初始状态清零，然后重试原指令。
     */
    if (!p->_used_lsx)
    {
      for (int index = 0; index < 32; ++index)
      {
        p->_trapframe->lsx[index][0] = p->_trapframe->f[index];
        p->_trapframe->lsx[index][1] = 0;
      }
    }
    p->_used_fpu = true;
    p->_used_lsx = true;
  }

  else if (is_loongarch_page_fault_code(ecode))
  {
    uint64 badv = r_csr_badv();
    mem::Pte fault_pte = p->get_pagetable()->walk(badv, false);
    if (fault_pte.is_null())
    {
      printfRed("usertrap(): badv=%p has null pte slot\n", badv);
    }

    // LoongArch 的软件 TLB 路径里，已映射用户页也可能因为 TLB 里残着 V=0 /
    // 旧权限状态而先打到 TLBL/TLBS/TLBI/TLBM。对于这种“PTE 已经合法存在”的页，
    // 先做一次按页失效，让硬件重走 refill；不要误丢进 mmap 懒分配分支。
    if (loongarch_can_retry_present_user_fault(fault_pte, ecode))
    {
      if (ecode == 0x4 && !fault_pte.is_dirty())
      {
        fault_pte.set_data(fault_pte.get_data() | loongarch::pte_dirty_m);
      }
      loongarch_invalidate_user_tlb_page(
          loongarch_user_asid(p->get_memory_manager()), badv);
      goto usertrap_page_fault_done;
    }

    if (mmap_handler(badv, ecode) != 0)
    {
      // 正常的惰性缺页不需要刷日志；只有补页失败时才展开上下文，方便定位真实异常。
      printfRed("usertrap(): page fault at %p, sending SIGSEGV to pid=%d\n", badv, p->_pid);
      printfYellow("usertrap(): fault pte=%p valid=%d present=%d user=%d read=%d write=%d exec=%d plv=%d\n",
                   (void *)fault_pte.get_data(),
                   (int)fault_pte.is_valid(),
                   (int)fault_pte.is_present(),
                   (int)!fault_pte.is_super_plv(),
                   (int)fault_pte.is_readable(),
                   (int)fault_pte.is_writable(),
                   (int)fault_pte.is_executable(),
                   (int)fault_pte.plv());
      printfYellow("usertrap(): regs ra=%p sp=%p fp=%p s0=%p s1=%p s2=%p s3=%p s4=%p t0=%p t1=%p a0=%p a1=%p a2=%p\n",
                   (void *)p->_trapframe->ra,
                   (void *)p->_trapframe->sp,
                   (void *)p->_trapframe->fp,
                   (void *)p->_trapframe->s0,
                   (void *)p->_trapframe->s1,
                   (void *)p->_trapframe->s2,
                   (void *)p->_trapframe->s3,
                   (void *)p->_trapframe->s4,
                   (void *)p->_trapframe->t0,
                   (void *)p->_trapframe->t1,
                   (void *)p->_trapframe->a0,
                   (void *)p->_trapframe->a1,
                   (void *)p->_trapframe->a2);
      p->add_signal(proc::ipc::signal::SIGSEGV);

      printf("usertrap(): unexpected trapcause 0x%x pid=%d ecode=%u esubcode=%u\n",
             estat, p->_pid, ecode, esubcode);
      printf("            era=%p badi=%x\n", r_csr_era(), r_csr_badi());
    }
  usertrap_page_fault_done:
    ;
  }
  else if (ecode == 0x8 || ecode == 0x9)
  {
    // LoongArch 手册：
    //   ecode=0x8, esubcode=0 => ADEF（取指地址错误）
    //   ecode=0x8, esubcode=1 => ADEM（访存地址错误）
    //   ecode=0x9            => ALE（地址对齐错误）
    // 这些都不是“缺页可补”的场景，直接按用户态同步地址错误送信号。
    printfRed("usertrap(): address error pid=%d ecode=%u esubcode=%u era=%p badv=%p badi=%x\n",
              p->_pid, ecode, esubcode, r_csr_era(), r_csr_badv(), r_csr_badi());
    printfYellow("usertrap(): address-error regs ra=%p sp=%p fp=%p s0=%p s1=%p s2=%p s3=%p s4=%p t0=%p t1=%p a0=%p a1=%p a2=%p\n",
                 (void *)p->_trapframe->ra,
                 (void *)p->_trapframe->sp,
                 (void *)p->_trapframe->fp,
                 (void *)p->_trapframe->s0,
                 (void *)p->_trapframe->s1,
                 (void *)p->_trapframe->s2,
                 (void *)p->_trapframe->s3,
                 (void *)p->_trapframe->s4,
                 (void *)p->_trapframe->t0,
                 (void *)p->_trapframe->t1,
                 (void *)p->_trapframe->a0,
                 (void *)p->_trapframe->a1,
                 (void *)p->_trapframe->a2);
    p->add_signal(proc::ipc::signal::SIGSEGV);
  }
  else if ((which_dev = devintr()) != 0)
  {
    // ok
  }
  else
  {
    printf("usertrap(): unexpected trapcause %x pid=%d\n", r_csr_estat(), p->_pid);
    printf("            era=%p badi=%x,badv=%p\n", r_csr_era(), r_csr_badi(), r_csr_badv());
    p->_killed = 1; // loongarch这里先不改, riscv的改为使用scuase判断信号 @todo
  }

  if (which_dev == 2 && p->_last_user_tick > 0 && cur_tick == p->_last_user_tick)
  {
    // LoongArch 这里和 RISC-V 一样，timer tick 是在 devintr() 里推进的。
    // 不补这一拍，用户态 CPU 时间不会随定时中断累计，VIRTUAL/PROF timer 会卡死。
    p->_user_ticks += 1;
    p->_kernel_entry_tick = tmm::get_ticks();
  }

  if (proc::ipc::signal::has_signal_pending(p, proc::ipc::signal::SIGKILL))
    proc::k_pm.do_signal_exit(p, proc::ipc::signal::SIGKILL);
  if (p->_killed)
    proc::k_pm.exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    if (Cpu::get_cpu()->advance_time_slice(k_default_time_slice_ticks))
    {
      proc::k_scheduler.yield();
    }
  }
  proc::ipc::signal::handle_signal(); // 处理信号 - 在返回用户态之前检查并处理待处理的信号
  usertrapret();
}

void trap_manager::usertrapret(void)
{
  //   printfCyan("==usertrapret== pid=%d\n", proc::k_pm.get_cur_pcb()->_pid);
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

  // CLONE_VM 线程共享 PGDL，但每个 PCB 有独立 trapframe 物理页。固定复用
  // TRAPFRAME 会让两个核并发返回用户态时覆盖同一 PTE；使用 PCB 槽位专属 VA，
  // 并将这个地址传给 userret/SAVE0 后即可让各线程完全隔离。
  intr_off();
  const uint64 user_trapframe_va = USER_TRAPFRAME(p->get_global_id());

  // 同一线程的大多数 syscall 不应反复拆装自己的 trapframe PTE；这不仅浪费
  // TLB，也会让多个 CLONE_VM 线程在第一次并发返回时竞争页表层级创建。仅在
  // PCB 槽位复用后物理页变化时替换映射，并以全局页表更新锁保护“检查+建表”。
  mem::PageTable *user_pt = p->get_pagetable();
  const uint64 expected_trapframe_pa =
      PGROUNDDOWN(to_phy(reinterpret_cast<uint64>(p->get_trapframe())));
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
      // PCB 槽位复用时可能仍留有历史映射；锁内仅替换当前槽位，其他线程的
      // trapframe PTE 不会被触碰。
      mem::k_vmm.vmunmap(*user_pt, user_trapframe_va, 1, 0);
      mapped = mem::k_vmm.map_pages(*user_pt,
                                    user_trapframe_va,
                                    PGSIZE,
                                    (uint64)p->get_trapframe(),
                                    PTE_V | PTE_NX | PTE_P | PTE_W | PTE_R | PTE_MAT | PTE_D);
    }
    mem::k_vmm.unlock_page_table_updates();
    if (!mapped)
    {
      panic("usertrapret: failed to map trapframe");
    }
  }
  // trapframe 是 trap 入口切换回内核页表前唯一需要读取的用户页表映射。
  // 保留 ASID+VA 精确失效作为 PCB/页表复用边界的最后一道保护；它不会像
  // 旧的 invtlb op0 那样清空本 CPU 的全部用户翻译。
  hal::tlb::enter_mm(*p->get_memory_manager());
  const uint32 user_asid = loongarch_user_asid(p->get_memory_manager());
  loongarch_invalidate_user_tlb_page(user_asid, user_trapframe_va);

  // send syscalls, interrupts, and exceptions to uservec.S
  w_csr_eentry((uint64)uservec); // maybe todo

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->get_trapframe()->kernel_pgdl = r_csr_pgdl();                // kernel page table
  p->get_trapframe()->kernel_sp = p->get_kstack() + KSTACK_SIZE; // process's kernel stack
  p->get_trapframe()->kernel_trap = (uint64)wrap_usertrap;
  //   printf("usertrapret: p->get_trapframe()->kernel_trap: %p\n", p->get_trapframe()->kernel_trap);
  // LoongArch 长跑下，tp 在极窄的 trap 返回窗口里可能暂时不是内核 cpuid。
  // 这里给下一次 uservec 入口保存的 hartid 必须直接来自 CSR_CPUID，
  // 否则后续再进内核时就可能把用户 TLS/栈附近的值误当成 current cpu 标识。
  p->get_trapframe()->kernel_hartid = r_csr_cpuid();

  // set up the registers that uservec.S's ertn will use
  // to get to user space.

  // set Previous Privilege mode to User Privilege3.
  uint32 x = r_csr_prmd();
  x |= PRMD_PPLV; // set PPLV to 3 for user mode
  x |= PRMD_PIE;  // enable interrupts in user mode
  w_csr_prmd(x);

  // set S Exception Program Counter to the saved user pc.
  w_csr_era(p->get_trapframe()->era);

  // tell uservec.S the user page table to switch to.
  volatile uint64 pgdl = (p->get_pagetable()->get_base());

  // jump to uservec.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with ertn.
  userret(user_trapframe_va,
          pgdl,
          (p->_used_fpu ? 1 : 0) | (p->_used_lsx ? 2 : 0),
          reinterpret_cast<uint64>(p->get_trapframe()),
          user_asid);
}
void trap_manager::machine_trap()
{
  panic("machine error");
}
// 处理内核态的中断
// 支持嵌套中断
void trap_manager::kerneltrap()
{
  // printf("==kerneltrap==\n");
  // 这些寄存器可能在yield时被修改
  int which_dev = 0;
  uint64 era = r_csr_era();
  uint64 prmd = r_csr_prmd();

  // 检查中断是否来自内核态

  if ((prmd & PRMD_PPLV) != 0)
    panic("kerneltrap: not from privilege0");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    /// TODO: pthread_cond_smasher 会在这里panic，但是不panic也没有什么问题，先暂时删掉这部分代码
  }

  ///@todo!! 写完进程后修改
  // give up the CPU if this is a timer interrupt.
  // 内核态长时间执行系统调用或缺页处理时也必须参与抢占；每 CPU 使用独立
  // time-slice 计数，避免一个核的 timer tick 影响其它核正在运行的任务。
  if (which_dev == 2 && Cpu::get_cpu()->get_cur_proc() != nullptr &&
      Cpu::get_cpu()->get_cur_proc()->_state == proc::RUNNING &&
      !Cpu::get_cpu()->get_cur_proc()->_exiting)
  {
    if (Cpu::get_cpu()->advance_time_slice(k_default_time_slice_ticks))
    {
      proc::k_scheduler.yield();
    }
  }
  // if (which_dev == 2)
  // {
  //   timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
  //   // printf("timeslice: %d\n", timeslice);
  //   if (timeslice >= 5)
  //   {
  //     timeslice = 0;
  //   }
  // }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_csr_era(era);
  w_csr_prmd(prmd);
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

  // 确定访问类型 (LoongArch 的异常码)
  int access_type = 0; // 默认读取
  if (cause == 2 || cause == 4)
  {                  // Store page fault
    access_type = 1; // 写入
  }
  else if (cause == 8 || cause == 3)
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

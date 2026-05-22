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
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern "C" void uservec();
extern "C" void handle_tlbr();
extern "C" void handle_merr();
extern "C" void userret(uint64, uint64);
int mmap_handler(uint64 va, int cause);
// 创建一个静态对象
trap_manager trap_mgr;

namespace
{
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
}

// 初始化锁
void trap_manager::init()
{
  ticks = 0;
  timeslice = 0;
  tickslock.init("tickslock");
  printfGreen("[trap] Trap Manager Init\n");
}

// 架构相关, 设置csr
void trap_manager::inithart()
{
  uint32 ecfg = (0U << CSR_ECFG_VS_SHIFT) | HWI_VEC | TI_VEC;
  // LoongArch 的 timer CSR 直接按周期数编程。这里必须与 tmm::cycles_per_tick()
  // 保持一致，否则 sleep()/CPU 计时/interval timer 会共同漂移，LTP 的
  // setitimer01 就会从毫秒级拖成几十秒。
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
  static bool uart_irq_warned = false;
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

  if (estat & ecfg & HWI_VEC)
  {
    // this is a hardware interrupt, via IOCR.

    // irq indicates which device interrupted.
    uint64 irq = extioi_claim();
    // printf("%d\n", irq);
    // 处理串口中断
    if (irq & (1UL << UART0_IRQ))
    {
      if (!uart_irq_warned)
      {
        uart_irq_warned = true;
        printfYellow("[trap] UART0 中断尚未接入驱动，先记录并继续运行\n");
      }
      apic_complete(1UL << UART0_IRQ);
      extioi_complete(1UL << UART0_IRQ);
    }
    else if (irq & (1UL << PCIE_IRQ))
    {
      // TODO
      // intr_stats::k_intr_stats.record_interrupt(PCIE_IRQ);
      // loongarch::qemu::disk_driver.handle_intr();
            if (!pcie_irq_warned)
      {
        pcie_irq_warned = true;
        printfYellow("[trap] PCIE 中断当前未走内核分发路径，先确认并放行\n");
      }
      apic_complete(1UL << PCIE_IRQ);
      extioi_complete(1UL << PCIE_IRQ);
      printfYellow("未实现PCIE_IRQ中断处理,不过好像跟riscv不一样，跟蒙老师也不一样，现在好像不用这个\n");
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);

      apic_complete(irq);
      extioi_complete(irq);
    }

    return 1;
  }
  else if (estat & ecfg & TI_VEC)
  {
    // timer interrupt.
    // 先清 pending，再做较重的 tick/唤醒/定时器检查，避免中途再进一次 trap
    // 时又把这同一个 pending timer 误当成“新的时钟中断”。
    w_csr_ticlr(r_csr_ticlr() | CSR_TICLR_CLR);

    if (proc::k_pm.get_cur_cpuid() == 0)
    {
      timertick();
    }

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
  tickslock.release();

  proc::k_pm.wakeup(&ticks);

  // Check for expired POSIX timers and send signals
  check_expired_timers();
  proc::check_interval_timers(proc::k_pm.get_cur_pcb());
}

// !!写完进程后修改
void trap_manager::usertrap()
{
  // printfMagenta("==usertrap==\n");

  int which_dev = 0;

  if ((r_csr_prmd() & PRMD_PPLV) == 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_csr_eentry((uint64)kernelvec);

  proc::Pcb *p = proc::k_pm.get_cur_pcb();

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

    if (p->_killed)
      proc::k_pm.exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->_trapframe->era += 4;

    // an interrupt will change crmd & prmd registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall::k_syscall_handler.invoke_syscaller();
  }

  else if (is_loongarch_page_fault_code(ecode))
  {
    mem::Pte fault_pte = p->get_pagetable()->walk(r_csr_badv(), false);
    if (fault_pte.is_null())
    {
      printfRed("usertrap(): badv=%p has null pte slot\n", r_csr_badv());
    }
    if (mmap_handler(r_csr_badv(), ecode) != 0)
    {
      // 正常的惰性缺页不需要刷日志；只有补页失败时才展开上下文，方便定位真实异常。
      printfRed("usertrap(): page fault at %p, sending SIGSEGV to pid=%d\n", r_csr_badv(), p->_pid);
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

  if (p->_killed)
    proc::k_pm.exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    if (timeslice >= 10)
    {
      timeslice = 0;
      // proc::ipc::signal::handle_signal();
      printf("yield in usertrap\n");
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

  // 统一在usertrapret时动态映射trapframe
  // 取消当前trapframe的映射
  mem::k_vmm.vmunmap(*p->get_pagetable(), TRAPFRAME, 1, 0);

  // 重新映射当前进程的trapframe
  if (mem::k_vmm.map_pages(*p->get_pagetable(), TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()),
                           PTE_V | PTE_NX | PTE_P | PTE_W | PTE_R | PTE_MAT | PTE_D) == 0)
  {
    panic("usertrapret: failed to dynamically map trapframe");
  }

  intr_off();

  // send syscalls, interrupts, and exceptions to uservec.S
  w_csr_eentry((uint64)uservec); // maybe todo

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->get_trapframe()->kernel_pgdl = r_csr_pgdl();                // kernel page table
  p->get_trapframe()->kernel_sp = p->get_kstack() + KSTACK_SIZE; // process's kernel stack
  p->get_trapframe()->kernel_trap = (uint64)wrap_usertrap;
  //   printf("usertrapret: p->get_trapframe()->kernel_trap: %p\n", p->get_trapframe()->kernel_trap);
  p->get_trapframe()->kernel_hartid = r_tp(); // hartid for cpuid()

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
  userret(TRAPFRAME, pgdl);
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
    uint32 estat = r_csr_estat();
    uint32 ecode = loongarch_exception_code(estat);
    uint32 esubcode = loongarch_exception_subcode(estat);
    uint64 badv = r_csr_badv();
    uint32 badi = r_csr_badi();
    uint32 crmd = r_csr_crmd();
    uint32 ecfg = r_csr_ecfg();
    uint64 save1 = r_csr_save1();
    uint64 save2 = r_csr_save2();
    uint64 extioi_isr = read_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_ISR_BASE);
    uint64 ls7a_status = *(volatile uint64 *)(LS7A_INT_STATUS_REG);
    uint64 heap_cache = 0;
    uint64 heap_used = 0;
    uint32 heap_chunks = 0;
    uint64 heap_free_pages = 0;
    uint32 heap_max_block_pages = 0;
    mem::k_hmm.get_stats(heap_cache, heap_used, heap_chunks, heap_free_pages, heap_max_block_pages);
    proc::Pcb *cur = Cpu::get_cpu()->get_cur_proc();

    if (cur != nullptr)
    {
      panic("kerneltrap: estat=%x ecode=%d esub=%d era=%p eentry=%p badv=%p badi=%x crmd=%x prmd=%x ecfg=%x save1=%p save2=%p pgdl=%p extioi=%p ls7a=%p proc=%s pid=%d tid=%d state=%d pt=%p mm=%p heap_used=%p heap_cache=%p heap_chunks=%d heap_free_pages=%p heap_max_block_bytes=%p",
            estat,
            ecode,
            esubcode,
            r_csr_era(),
            r_csr_eentry(),
            (void *)badv,
            badi,
            crmd,
            prmd,
            ecfg,
            (void *)save1,
            (void *)save2,
            (void *)r_csr_pgdl(),
            (void *)extioi_isr,
            (void *)ls7a_status,
            cur->_name,
            cur->_pid,
            cur->_tid,
            cur->_state,
            cur->get_pagetable(),
            cur->get_memory_manager(),
            (void *)heap_used,
            (void *)heap_cache,
            heap_chunks,
            (void *)heap_free_pages,
            (void *)(static_cast<uint64>(heap_max_block_pages) * PGSIZE));
    }

    panic("kerneltrap: estat=%x ecode=%d esub=%d era=%p eentry=%p badv=%p badi=%x crmd=%x prmd=%x ecfg=%x save1=%p save2=%p pgdl=%p extioi=%p ls7a=%p no-current-proc heap_used=%p heap_cache=%p heap_chunks=%d heap_free_pages=%p heap_max_block_bytes=%p",
          estat,
          ecode,
          esubcode,
          r_csr_era(),
          r_csr_eentry(),
          (void *)badv,
          badi,
          crmd,
          prmd,
          ecfg,
          (void *)save1,
          (void *)save2,
          (void *)r_csr_pgdl(),
          (void *)extioi_isr,
          (void *)ls7a_status,
          (void *)heap_used,
          (void *)heap_cache,
          heap_chunks,
          (void *)heap_free_pages,
          (void *)(static_cast<uint64>(heap_max_block_pages) * PGSIZE));
  }

  ///@todo!! 写完进程后修改
  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && Cpu::get_cpu()->get_cur_proc() != nullptr && Cpu::get_cpu()->get_cur_proc()->_state == proc::RUNNING)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    if (timeslice >= 5)
    {
      timeslice = 0;
      printf("yield in kerneltrap\n");
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
  int i;
  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  // 根据地址查找属于哪一个VMA
  for (i = 0; i < proc::NVMA; ++i)
  {
    if (p->get_vma()->_vm[i].used)
    {
      // 检查是否在当前VMA范围内
      if (va >= p->get_vma()->_vm[i].addr && va < p->get_vma()->_vm[i].addr + p->get_vma()->_vm[i].len)
      {
        printfGreen("mmap_handler: found VMA %d for va %p\n", i, va);
        break; // 在当前VMA范围内
      }
    }
  }

  if (i == proc::NVMA)
  {
    printfRed("mmap_handler: no VMA found for va %p\n", va);
    return -1;
  }

  // 获取VMA结构
  struct proc::vma *vm = &p->get_vma()->_vm[i];

  // 确定访问类型 (LoongArch的异常码)
  int access_type = 0; // 默认读取
  if (cause == 2)
  {                  // Store page fault
    printfOrange("[mmap_handler] Store page fault at va: %p\n", va);
    access_type = 1; // 写入
  }
  else if (cause == 8||cause == 3)
  {                  // Instruction page fault
    printfOrange("[mmap_handler] Instruction page fault at va: %p\n", va);
    access_type = 2; // 执行
  }
  else{
    printfRed("[mmap_handler] Load page fault cause: %d at va: %p\n", cause, va);
  }

  // 使用统一的VMA页面分配函数
  return mem::k_vmm.allocate_vma_page(*p->get_pagetable(), va, vm, access_type);
}
#endif

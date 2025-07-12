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
#include "devs/riscv/disk_driver.hh"
#include "proc.hh"
#include "mem.hh"
#include "physical_memory_manager.hh"
#include "fs/vfs/file/normal_file.hh"
#include "virtual_memory_manager.hh"
#include "timer_interface.hh"
#include "timer_manager.hh"

// #include "fuckyou.hh"
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern char trampoline[], uservec[], userret[];
// 创建一个静态对象
trap_manager trap_mgr;

// 前置声明，内部函数只有这里使用。
int mmap_handler(uint64 va, int cause);

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
  w_stvec((uint64)kernelvec);
  w_sstatus(r_sstatus() | SSTATUS_SIE);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
  set_next_timeout();
  printfGreen("[trap] Trap Manager Inithart\n");
}

// 时钟到期后, 重新设置下次超时
void trap_manager::set_next_timeout()
{

  sbi_set_timer(r_time() + INTERVAL);
}

// 处理外部中断和软件中断
int trap_manager::devintr()
{
  uint64 scause = r_scause();

  if ((scause & 0x8000000000000000L) &&
      (scause & 0xff) == 9)
  {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_mgr.claim();

    if (irq == UART0_IRQ)
    {
      // 现在只写了接收中断, 没有发送中断
      // printf("uart0 interrupt\n");
      int c = sbi_console_getchar();
      if (-1 != c)
      {
        dev::kConsole.console_intr(c);
      }
    }
    //!!写完磁盘后修改
    else if (irq == VIRTIO0_IRQ)
    {
      riscv::qemu::disk_driver.handle_intr();
    }
    else if (irq == VIRTIO1_IRQ)
    {
      panic("virtio1 interrupt not handled yet");
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
    timertick();

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
  // acquire the lock to protect ticks
  tickslock.acquire();

  // increment the ticks count
  ticks++;
  proc::k_pm.wakeup(&ticks);

  // printfCyan("[tm]  timertick here,p->addr:%x \n",Cpu::get_cpu()->get_cur_proc());
  // release the lock
  tickslock.release();

  // set the next timeout
  set_next_timeout();
}

// 处理内核态的中断
// 支持嵌套中断
void trap_manager::kerneltrap()
{
  // printfMagenta("into kerneltrap\n");
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
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  if (which_dev == 2 && Cpu::get_cpu()->get_cur_proc() != nullptr && Cpu::get_cpu()->get_cur_proc()->_state == proc::RUNNING)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    // printf("timeslice: %d\n", timeslice);
    if (timeslice >= 5)
    {
      printfCyan("[kerneltrap]  yield here,p->addr:%x \n",Cpu::get_cpu()->get_cur_proc());
      proc::k_scheduler.yield();
      timeslice = 0;
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
  w_stvec((uint64)kernelvec);

  proc::Pcb *p = proc::k_pm.get_cur_pcb();
  p->_trapframe->epc = r_sepc();
  uint64 cause = r_scause();
  if (cause == 8)
  {
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
  else if (cause == 13 || cause == 15)
  {
    // 缺页故障处理
    TODO("pagefault_handler");
    ///@brief 此处处理mmap的缺页异常
    // printfRed("p->_trapframe->sp: %p,printf fault_va:%p, p->_sz:%p\n", PGROUNDUP(p->_trapframe->sp) - 1, fault_va, p->_sz);

    if (mmap_handler(r_stval(), cause) != 0)
    {
      printfRed("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->_pid);
      printfRed("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->_killed = 1;
    }
  }
  else
  {
    printfRed("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->_pid);
    printfRed("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->kill();
  }

  if (p->is_killed())
    proc::k_pm.exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    if (timeslice >= 5)
    {
      timeslice = 0;
      // 处理信号 - 在返回用户态之前检查并处理待处理的信号
      proc::ipc::signal::handle_signal();
      printf("yield in usertrap\n");
      proc::k_scheduler.yield();
    }
  }
  // printfMagenta("left usertrap\n");
  usertrapret();
}

void trap_manager::usertrapret()
{
  // printfMagenta("into usertrapret\n");
  proc::Pcb *p = proc::k_pm.get_cur_pcb();
  // Debug
  //  printfYellow("[usertrapret] trampoline addr %p\n", trampoline);


  // 检查进程是否使用共享虚拟内存，如果是则需要动态映射trapframe
  if (p->_shared_vm || p->_pt.get_ref_count() > 1) {
    // 页表被共享，需要动态重新映射trapframe
    // printfCyan("[usertrapret] Page table shared (ref count: %d), dynamically mapping trapframe for pid %d\n", 
            //    p->_pt.get_ref_count(), p->_pid);
    
    // 取消当前trapframe的映射
    mem::k_vmm.vmunmap(p->_pt, TRAPFRAME, 1, 0);
    
    // 重新映射当前进程的trapframe
    if (mem::k_vmm.map_pages(p->_pt, TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()), 
                             riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_writable_m) == 0)
    {
      panic("usertrapret: failed to dynamically map trapframe");
    }
  }
  mem::Pte pte = p->_pt.walk(TRAMPOLINE, 0);
  if (pte.is_null() || pte.is_valid() == 0)
  {
    panic("trampoline not mapped in user pagetable!");
  }

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();
  w_stvec(TRAMPOLINE + (uservec - trampoline));
  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->_trapframe->kernel_satp = r_satp();
  p->_trapframe->kernel_sp = p->_kstack + 1 * PGSIZE;
  p->_trapframe->kernel_trap = (uint64)wrap_usertrap;
  p->_trapframe->kernel_hartid = r_tp();

  uint64 x = r_sstatus();
  x &= ~riscv::csr::sstatus_spp_m;
  x |= riscv::csr::sstatus_spie_m;
  w_sstatus(x);
  w_sepc(p->_trapframe->epc);

  // printfYellow("[usertrapret] sepc: %p,saved sepc:%p\n", p->_trapframe->epc,r_sepc());
  // tell trampoline.S the user page table to switch to.

  // debug
  // printfYellow("[usertrapret]user pagetable addr: %p\n", p->_pt.get_base());

  uint64 satp = MAKE_SATP(p->_pt.get_base());
  // debug

  uint64 fn = TRAMPOLINE + (userret - trampoline);
  // printf("trapframe addr: %p\n", p->_trapframe);
//   printf("trapframe->epc: %p\n", p->_trapframe->epc);
//   printf("[usertrapret] trapframe->a0: %p\n", p->_trapframe->a0);
  
  ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);
  // !! 这个地方应该是固定值, 如果上多核的时候出错的话, 就改回下面
  // ((void (*)(uint64, uint64))fn)(TRAPFRAME + proc::k_pm.get_cur_cpuid() * sizeof(TrapFrame), satp);
}

/**
 * @brief mmap_handler 处理mmap惰性分配导致的页面错误
 * @param va 页面故障虚拟地址
 * @param cause 页面故障原因
 * @return 0成功，-1失败
 */
int mmap_handler(uint64 va, int cause)
{
  int i;
  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  // 根据地址查找属于哪一个VMA
  for (i = 0; i < proc::NVMA; ++i)
  {
    if (p->_vma->_vm[i].used)
    {

      // 检查是否在当前VMA范围内
      if (va >= p->_vma->_vm[i].addr && va < p->_vma->_vm[i].addr + p->_vma->_vm[i].len)
      {

        break; // 在当前VMA范围内
      }
      // 检查是否可以扩展
      else if (p->_vma->_vm[i].is_expandable &&
               va < p->_vma->_vm[i].addr + p->_vma->_vm[i].max_len)
      {
        // 扩展当前VMA
        uint64 new_len = PGROUNDUP(va - p->_vma->_vm[i].addr + PGSIZE);
        if (new_len <= p->_vma->_vm[i].max_len)
        {
          p->_vma->_vm[i].len = new_len;
          p->_sz += (new_len - p->_vma->_vm[i].len);
          break;
        }
      }
    }
  }
  if (i == proc::NVMA)
    return -1;
  int pte_flags = PTE_U;
  if (p->_vma->_vm[i].prot == 0)
  {
    pte_flags |= PTE_R | PTE_X | PTE_W;
    printfYellow("mmap_handler: prot=0 mapping, using minimal permissions\n");
  }
  else
  {
    if (p->_vma->_vm[i].prot & PROT_READ)
      pte_flags |= PTE_R;
    if (p->_vma->_vm[i].prot & PROT_WRITE)
      pte_flags |= PTE_W;
    if (p->_vma->_vm[i].prot & PROT_EXEC)
      pte_flags |= PTE_X;
  }
  fs::normal_file *vf = p->_vma->_vm[i].vfile;

  void *pa = mem::k_pmm.alloc_page();
  if (pa == nullptr)
  {
    printfRed("mmap_handler: alloc_page failed\n");
    return -1; // 分配页面失败
  }
  if (pa == 0)
    return -1;
  memset(pa, 0, PGSIZE);

  // 检查是否为匿名映射
  if (vf == nullptr || p->_vma->_vm[i].vfd == -1)
  {
    // 匿名映射：页面已经初始化为0，直接映射即可
    // printfCyan("mmap_handler: handling anonymous mapping at %p\n", va);
  }
  else
  {
    panic("未实现");
#ifdef FS_FIX_COMPLETELY
    // 文件映射：需要从文件读取内容
    fs::dentry *den = vf->getDentry();
    if (den == nullptr)
    {
      printfRed("mmap_handler: dentry is null\n");
      mem::k_pmm.free_page(pa);
      return -1; // dentry is null
    }
    fs::Inode *inode = den->getNode();
    if (inode == nullptr)
    {
      printfRed("mmap_handler: inode is null\n");
      mem::k_pmm.free_page(pa);
      return -1; // inode is null
    }

    inode->_lock.acquire(); // 锁定inode，防止其他线程修改

    // 计算当前页面读取文件的偏移量，实验中p->_vma->_vm[i].offset总是0
    // 要按顺序读读取，例如内存页面A,B和文件块a,b
    // 则A读取a，B读取b，而不能A读取b，B读取a
    int offset = p->_vma->_vm[i].offset + PGROUNDDOWN(va - p->_vma->_vm[i].addr);
    ///@details 原本的xv6的readi函数有一个标志位来区分是否读到内核中，此处位于内核里
    /// pa直接是物理地址，所以应该无所谓
    int readbytes = inode->nodeRead((uint64)pa, offset, PGSIZE);

    // 什么都没有读到
    if (readbytes == 0)
    {
      printfRed("mmap_handler: read nothing");
      inode->_lock.release();
      mem::k_pmm.free_page(pa);
      return -1;
    }
    inode->_lock.release(); // 释放inode锁
    // printfCyan("mmap_handler: handling file mapping at %p, read %d bytes\n", va, readbytes);
#endif
  }

  // 添加页面映射
  if (mem::k_vmm.map_pages(*p->get_pagetable(), PGROUNDDOWN(va), PGSIZE, (uint64)pa, pte_flags) != 1)
  {
    printfRed("mmap_handler: map failed");
    mem::k_pmm.free_page(pa);
    return -1;
  }

  return 0;
}
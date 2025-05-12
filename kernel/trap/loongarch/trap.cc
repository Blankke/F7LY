#include "types.hh"
#include "trap.hh"
#include "platform.hh"
#include "param.h"
#include "plic.hh"
#include "mem/memlayout.hh"
#include "devs/console.hh"
#include "printer.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "trap_func_wrapper.hh"
#include "extioi.hh"
#include "pci.hh"
#include "apic.hh"
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern "C" void uservec();
extern "C" void handle_tlbr();
extern "C" void handle_merr();
extern "C" void userret(uint64, uint64);
// 创建一个静态对象
trap_manager trap_mgr;

// 初始化锁
void trap_manager::init()
{
  ticks = 0;
  timeslice = 0;
  tickslock.init("tickslock");
}

// 架构相关, 设置csr
void trap_manager::inithart()
{
  uint32 ecfg = ( 0U << CSR_ECFG_VS_SHIFT ) | HWI_VEC | TI_VEC;
  uint64 tcfg = 0x1000000UL | CSR_TCFG_EN | CSR_TCFG_PER;

  w_csr_ecfg(ecfg);
  w_csr_tcfg(tcfg);

  w_csr_eentry((uint64)kernelvec);
  w_csr_tlbrentry((uint64)handle_tlbr);
  w_csr_merrentry((uint64)handle_merr);
  intr_on();
  set_next_timeout();
}

// 时钟到期后, 重新设置下次超时
void trap_manager::set_next_timeout()
{
  TODO("设置下次超时的龙芯做法");
}

// 处理外部中断和软件中断
int trap_manager::devintr()
{

  uint32 estat = r_csr_estat();
  uint32 ecfg = r_csr_ecfg();

  if(estat & ecfg & HWI_VEC) {
    // this is a hardware interrupt, via IOCR.

    // irq indicates which device interrupted.
    uint64 irq = extioi_claim();
    // printf("%d\n", irq);
    // 处理串口中断
    if(irq & (1UL << UART0_IRQ)){
      printfYellow("uart0 interrupt not implemented yet\n");
      //TODO kConsole.console_intr();


    // tell the apic the device is
    // now allowed to interrupt again.

      extioi_complete(1UL << UART0_IRQ);
    } else if (irq & (1UL << PCIE_IRQ)) {
      //TODO: virtio_disk_intr();  
    }else if(irq){
       printf("unexpected interrupt irq=%d\n", irq);

      apic_complete(irq);
      extioi_complete(irq);
    }

    return 1;
  } else if(estat & ecfg & TI_VEC){
    //timer interrupt,

    if(proc::k_pm.get_cur_cpuid() == 0){
      //TODO: clockintr();
    }

    // acknowledge the timer interrupt by clearing
    // the TI bit in TICLR.
    w_csr_ticlr(r_csr_ticlr() | CSR_TICLR_CLR);

    return 2;
  } else {
    return 0;
  }
}

void trap_manager::timertick()
{
  // acquire the lock to protect ticks
  tickslock.acquire();

  // increment the ticks count
  ticks++;

  // !!写完进城后修改
  // wakeup(&ticks);

  // release the lock
  tickslock.release();

  // set the next timeout
  set_next_timeout();
}

// !!写完进程后修改
// void trap_manager::usertrap(){
//   if((r_sstatus() & SSTATUS_SPP) != 0)
//     panic("usertrap: not from user mode");

//   // send interrupts and exceptions to kerneltrap(),
//   // since we're now in the kernel.
//   w_stvec((uint64)kernelvec);

//   struct proc *p = myproc();

//   // save user program counter.
//   p->trapframe->epc = r_sepc();
//   uint64 cause = r_scause();
//   if(cause == 8){
//     // system call

//     if(killed(p))
//       exit(-1);

//     // sepc points to the ecall instruction,
//     // but we want to return to the next instruction.
//     p->trapframe->epc += 4;

//     // an interrupt will change sepc, scause, and sstatus,
//     // so enable only now that we're done with those registers.
//     intr_on();

//     syscall();
//   } else if((which_dev = devintr()) != 0){
//     // ok
//   } else if (cause == 13 || cause == 15) {
//     //缺页故障处理
//     uint64 fault_va = r_stval();
//     if(PGROUNDUP(p->trapframe->sp) - 1 < fault_va && fault_va < p->sz) {
//       if(pagefault_handler(fault_va, cause) != 0) {
//         p->killed = 1;
//       }
//     } else
//       p->killed = 1;
//   } else {
//     printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
//     printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
//     setkilled(p);
//   }

//   if(killed(p))
//     exit(-1);

//   // give up the CPU if this is a timer interrupt.
//   if(which_dev == 2) {
//     timeslice++; //让一个进程连续执行若干时间片，printf线程不安全
//     if(timeslice >= 5){
//       timeslice = 0;
//       yield();
//     }

// }

// !!写完进程后修改
// return to user space
//
// void
// usertrapret(void)
// {
//   struct proc *p = myproc();

//   // we're about to switch the destination of traps from
//   // kerneltrap() to usertrap(), so turn off interrupts until
//   // we're back in user space, where usertrap() is correct.
//   intr_off();

//   w_stvec(TRAMPOLINE + (uservec - trampoline));

//   // set up trapframe values that uservec will need when
//   // the process next re-enters the kernel.
//   p->trapframe->kernel_satp = r_satp();         // kernel page table
//   p->trapframe->kernel_sp = p->kstack + 1 * PGSIZE; // process's kernel stack
//   p->trapframe->kernel_trap = (uint64)usertrap;
//   p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

//   // set up the registers that trampoline.S's sret will use
//   // to get to user space.

//   // set S Previous Privilege mode to User.
//   unsigned long x = r_sstatus();
//   x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
//   x |= SSTATUS_SPIE; // enable interrupts in user mode
//   w_sstatus(x);

//   // set S Exception Program Counter to the saved user pc.
//   w_sepc(p->trapframe->epc);

//   // tell trampoline.S the user page table to switch to.
//   // printf("[usertrapret]p->pagetable: %p\n", p->pagetable);
//   uint64 satp = MAKE_SATP(p->pagetable);

//   // jump to trampoline.S at the top of memory, which
//   // switches to the user page table, restores user registers,
//   // and switches to user mode with sret.
//   uint64 fn = TRAMPOLINE + (userret - trampoline);
//   ((void (*)(uint64,uint64))fn)(TRAPFRAME + cpuid() * sizeof(struct trapframe), satp);
// }

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

  if((prmd & PRMD_PPLV) != 0)
    panic("kerneltrap: not from privilege0");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("estat %x\n", r_csr_estat());
    printf("era=%p eentry=%p\n", r_csr_era(), r_csr_eentry());
    panic("kerneltrap");
  }

  //!! 写完进程后修改
  // give up the CPU if this is a timer interrupt.
  // if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING) {
  //   timeslice++; //让一个进程连续执行若干时间片，printf线程不安全
  //   if(timeslice >= 5){
  //     timeslice = 0;
  //     yield();
  //   }
  // }
  if (which_dev == 2)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    printf("timeslice: %d\n", timeslice);
    if (timeslice >= 5)
    {
      timeslice = 0;
    }
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_csr_era(era);
  w_csr_prmd(prmd);
}

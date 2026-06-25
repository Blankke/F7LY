= 第三章　中断管理器

== 3.1 整体设计
从用户态到内核态的切换需要中断和异常的频繁处理，此处F7LY在xv6的trap框架基础上选择了对象化中断管理器类TrapManager，并实例化全局对象trap_mgr。

```cpp
class trap_manager
{
friend  class tmm::TimerManager;
public:
    void init();
    void inithart();
    int devintr();
    void usertrap();    
    void usertrapret(); 
    void kerneltrap();  
private:
    void timertick();   
    void set_next_timeout(); 
    SpinLock tickslock; 
    uint ticks;        
    uint timeslice;    
};
```

F7LY 在其底层根据 RISC-V 和 loongarch 的不同硬件设备使用而封装了不同的中断处理器，在不同文件夹下实现了 PLIC、EXTIOI、APIC 的驱动、中断统计管理器和统一的包装函数层并应用在中断处理之中。

== 3.2 RISC-V 中断处理路径

*RISC-V* 使用 `PLIC` 单层中断控制器。用户态异常从 `uservec.S` 入口，保存寄存器与 `FPU` 现场后切换内核页表，进入`usertrap()` 统一分发；内核态异常走 `kernelvec.S` 进入 `kerneltrap()` 处理设备中断和时钟抢占。返回时由`usertrapret()` 设置 `trapframe`，跳转 `userret` 恢复寄存器后 `sret` 返回用户态。控制台输入走`SBI`，`uservec` 新增 `FPU` 现场保存。

== 3.3 LoongArch 中断处理路径

*LoongArch* 使用 `APIC` + `ExtIOI` 两级中断控制器。三个异常入口`kernelvec`、`handle_tlbr`、`handle_merr` 分别由 `eentry`/`tlbrentry`/`merrentry` 三个 CSR 指定。用户态和内核态异常均从 `kernelvec.S` 入口，保存寄存器后从 `CSR_CPUID` 重取 `hartid`，进入 `usertrap()` 按`ecode` + `esubcode` 二级编码分发；返回时由 `usertrapret()` 原子重映射trapframe，经 `ertn` 回到用户态。与 RISC-V 关键差异在于它的两级中断控制器、时钟中断 `TICLR` 直接写清除位、TLB 软件走表。

== 3.4 陷阱分发逻辑

`usertrap()` 进入后首先检查是否来自用户态，然后读取异常编码区分陷阱类型：

- 系统调用：
  - 检查进程是否被标记为结束。
  - 将返回地址指向 `ecall` 下一条指令。
  - 开中断，进入 `invoke_syscaller()` 查表分发。

- 缺页异常：
  - 先尝试 COW 解决共享写时复制页。
  - LA 额外检查 PTE 是否已合法，若只是 TLB 残留则做精准失效重试。
  - 进入 `mmap_handler()` 尝试惰性分配。
  - 以上均失败则发送 `SIGSEGV`。

- 地址错误：
  - 直接发送 `SIGSEGV`。

- 其他同步异常（非法指令、断点、对齐错误等）：
  - 按编码分别发送 `SIGILL`、`SIGBUS`、`SIGTRAP`、`SIGSYS` 等信号。

- FPU 禁用（LA 专属，`ecode=0xF`）：
  - 标记当前线程需要 FPU，返回用户态时懒恢复现场，不在此处保存浮点寄存器。

- 设备中断：
  - 进入 `devintr()`。先确保当前为中断场景（RISC-V 检查 `scause` 高位，
    LA 检查 `ecode==0`），再按 pending bit 分发：硬件中断通过 PLIC/ExtIOI
    认领并路由到设备驱动，时钟中断推进 `ticks` 并检查 POSIX 定时器。

所有分支处理完毕后，如有待处理信号则进入 `handle_signal()`。若本次为时钟
中断，`timeslice` 自增并在达到阈值时调用 `yield()` 触发调度。最后通过
`usertrapret()` 返回用户态。

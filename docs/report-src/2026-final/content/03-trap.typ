= 中断与陷阱处理

决赛阶段的中断改动围绕一个边界展开：架构代码识别 trap、保存和恢复上下文，平台中断控制器负责取出并结束中断，设备驱动负责判断自己的状态并处理事件，公共 IRQ 层只负责登记和分发。这样可以同时覆盖 RISC-V PLIC、LoongArch 平台中断控制器、定时器和跨核 IPI，而不在 trap 入口堆叠板级设备分支。

== 统一中断分发模型

=== source 注册与 owner 约束

设备初始化时先通过 `register_handler(source, handler, context, name)` 登记中断源及其所有者。IRQ registry 保存有限数量的共享处理函数，允许 PCI INTx 等场景由多个设备共享 source；控制器初始化时只启用已经登记的 source。重复登记同一 handler/context 会被视为幂等操作，未登记或超出平台能力的 source 不会被静默打开。

=== 确认来源、调用处理函数并结束中断

架构 trap 收到外部中断后只调用公共 `hal::irq::dispatch()`。该函数先从当前平台 backend 确认是哪一个中断源发出请求，再找到这个中断源对应的处理函数并调用它，最后使用同一次确认得到的信息通知控制器“处理完成”。处理函数必须自行确认设备状态；公共层不再按 UART、块设备或网卡写硬编码分支。

这种设计有两个不变量：一是 claim 与 complete 属于同一控制器事务，不能根据处理后的 pending 位图反推 token；二是没有 handler 的 source 只能记录一次诊断并完成控制器事务，不能让一个未知中断永久占住外部中断入口。

这两个不变量分别保障中断处理的正确性和系统活性：前者防止使用错误的完成参数破坏控制器状态，后者防止未处理的中断长期处于 pending 或 in-service 状态，进而阻塞后续设备中断、形成中断风暴或使系统卡死。

下面的代码省略了诊断和边界检查，只保留分发主流程：

```cpp
void dispatch() {
  const ClaimToken token = backend::claim();
  uint64 pending = token.pending_sources;
  while (pending != 0) {
    Source source = first_set_source(pending);
    for (auto &slot : handlers[source])
      if (slot.handler != nullptr)
        slot.handler(slot.context);
    pending &= pending - 1;
  }
  if (token.controller_token != 0)
    backend::complete(token);
}
```

控制器返回的中断标识会原样用于结束这次中断，因此设备处理函数改变 pending 状态也不会破坏控制器状态；没有对应处理函数时仍会执行结束操作。

=== 全局控制器与每核上下文

IRQ 初始化拆为两步。boot CPU 只执行一次全局控制器初始化、已登记 source 的启用和 registry 状态发布；每个 CPU 再初始化自己的控制器 context、优先级、timer 和本地中断状态。重复执行全局初始化或在控制器未就绪时初始化 CPU context 都会直接报告错误，避免多核启动阶段出现重复清零和部分启用。

== RISC-V 中断路径

=== 外部中断与 PLIC context

RISC-V 的 PLIC 由平台 backend 按当前 hart 配置 threshold、enable 和 claim/complete 寄存器。QEMU 画像使用固定 VirtIO/串口资源，VisionFive2 则从 DTB 的 `interrupts-extended` 建立 hart 到 PLIC context 的映射。这样，secondary hart 上的设备中断可以沿同一 dispatch 路径进入公共驱动。

=== 定时器与系统调用返回

timer interrupt 在架构 trap 中先确认来源，再推进系统 ticks、唤醒睡眠任务并处理已武装的 interval/POSIX timer。没有已武装定时器时不再扫描所有 PCB 或全局定时器表，降低多核空闲 tick 的固定开销。用户态 syscall trap 返回前保留架构要求的 PC 调整和 trapframe 状态，异常路径则转入信号或进程退出处理。

=== 外部中断后的现场恢复

RISC-V trap 入口保存通用寄存器，公共处理完成后恢复当前线程的用户 trapframe。共享地址空间的线程使用 PCB 槽位专属的 TRAPFRAME 映射，避免不同 CPU 同时返回用户态时写入同一个现场页。页表修改需要通知仍在使用该 mm 的 CPU，不能对所有 CPU 盲目执行全量失效。

RISC-V 架构层只判断中断类别，设备编号交给 IRQ backend：

```cpp
int devintr() {
  uint64 cause = r_scause();
  if (is_software_interrupt(cause)) {
    sbi_clear_ipi();
    return kWakeupIpi;
  }
  if (is_external_interrupt(cause)) {
    hal::irq::dispatch();
    return kDeviceInterrupt;
  }
  if (is_timer_interrupt(cause)) {
    set_next_timeout();
    if (Cpu::is_bootstrap_cpu()) timertick();
    check_expired_timers();
    return kTimerInterrupt;
  }
  return kNotDeviceInterrupt;
}
```

这样，trap 代码只负责区分软件、外部和定时器中断，PLIC 的 source 编号以及 UART、VirtIO 等设备的状态判断都留在平台和驱动层。

== LoongArch 中断路径

=== 区分异常和外部中断

LoongArch 的 `ESTAT` 同时记录异常类型和等待处理的中断。trap 处理先看异常类型：系统调用、页错误和地址错误进入各自的同步异常路径；确认确实是外部中断后，才继续分发定时器、设备或 IPI。这样不会把页错误、地址错误或系统调用误判成 timer/设备中断。用户 syscall 使用 `era += 4` 返回，页错误则进入统一缺页路径，无法修复时转换为 SIGSEGV。

=== CPU 槽位、IPI 与定时器

LoongArch 的内核 CPU 标识使用 CSR_CPUID，避免读取用户 TLS 后得到错误的 CPU 槽位。定时器 pending 先清除，再在锁外执行唤醒、POSIX timer 到期和调度请求。跨核唤醒通过平台 IPI 通知目标 CPU；IPI 只传递“重新检查 runnable/等待条件”的语义，不在中断处理器中直接持有进程或文件系统的长时间锁。

=== 用户态返回与现场恢复

LoongArch 返回用户态时按当前线程动态映射 trapframe，并完成返回所需的本地地址转换状态刷新。涉及 `CLONE_VM` 的页表一致性和跨核 TLB shootdown 属于内存管理问题，在第四章结合 mm 生命周期和 ASID 说明。

== 异常、设备中断与可睡眠路径

=== 同步异常的统一入口

系统调用、页错误、非法地址和未处理异常都先在架构 trap 层完成编码解析，再进入公共进程或内存服务。trap 入口不执行可能睡眠的文件系统或用户内存操作；需要缺页补页、信号分发或调度时，由当前线程在可睡眠上下文中继续处理。

=== 中断上下文的锁边界

中断处理器只做状态采样、唤醒请求和短事务，不持有 futex、mount、bcache 等可能跨 CPU 反向获取 PCB 锁的全局锁。定时器、IPI 和设备 handler 通过 sleep/wakeup 或 runnable 标志把工作交给调度器，避免在中断上下文中执行长时间 I/O。

== 验证结果

- RISC-V/LoongArch 四种构建均通过；QEMU `-smp 8 -m 8G` 启动后 CPU 视图、IPI、定时器和设备中断保持可用。
- timer_delete/gettime/settime、clock_settime、getitimer 与 `epoll_wait04` 连续回归共 19 项返回 0；futex 语义小集合和多核 IPI 定向场景无 panic 或死锁。


== 本章小结

本阶段将中断处理从架构和设备分支的集合，收敛为“架构 trap 解析—平台 controller backend—公共 IRQ registry—设备 handler”的路径，并补上多核 timer、IPI 和异常返回的生命周期管理。

= 第二章　机器启动

F7LY-OS 支持双架构启动，机器启动的源文件分别在 `kernel/boot/riscv/` 和`kernel/boot/loongarch/` 目录下。

整个 kernel 可执行文件的入口点统一为 `_entry`，在各自架构的 `entry.S` 中定义，通过链接脚本 `kernel/link/<arch>/kernel.ld` 的 `ENTRY(_entry)` 指定。

*RISC-V 架构*：
启动流程：`OpenSBI`（M-mode）→ `entry.S` → `start.cc` → `main()`。基于SBI（Supervisor Binary Interface）规范，Bootloader 工作在 M-mode，内核代码从 `entry.S` 开始运行在 S-mode。

*LoongArch 架构*：
启动流程：`QEMU/EDK2` → `entry.S` → `main()`（无 `start.cc` 薄层）。采用LoongArch 原生启动方式，通过 DTB 进行硬件发现与初始化。

== 2.1　RISC-V 启动流程

=== 2.1.1 entry.S


- 负责设置操作系统的栈指针`sp`到合适位置，为后续C++代码执行做准备。
- 每个CPU核心分配4KB独立栈空间，通过`hartid`进行区分。
- 完成栈设置后跳转到`start()`函数。

```c
_entry:
    la sp, stack0          # Load base addr of stack
    li t0, 1024*4          # 4KB space per stack
    mv t1, a0              # gain hartid
    addi t1, t1, 1
    mul t0, t0, t1         # cal current offset of stack
    add sp, sp, t0         # set stack pointer
    call start             # jump to start func
```

=== 2.1.2 start.cc

// 正文待补：satp = 0 关 MMU；stvec = trap_loop 兜底；tp = hartid；
// 调用 main(hartid, dtb_entry)；
// 变化：sie 早期初始化注释掉，统一交给 trap_mgr.inithart()；
// dtb_entry 被 PMM 用于安全上限截断；
// 控制台输入改为全走 SBI。
关键寄存器处理：
- `a0`：存储硬件线程编号`hartid`。
- `a1`：设备树地址信息dtb_entry  。
- `sp`：已在entry.S中设置完成。

#text()[#h(2em)]主要工作：
- 关闭分页机制，使用物理地址访问
- 设置临时trap处理函数为死循环
- 将hartid保存到tp寄存器供后续使用
- 调用main()函数进行系统初始化

```c
void start(uint64 hartid, uint64 dtb_entry)
{
    riscv::csr::_write_csr_(riscv::csr::satp, 0);
    riscv::csr::_write_csr_(riscv::csr::stvec, (uint64)trap_loop);
    riscv::w_tp(hartid);
    main(hartid, dtb_entry);
}
```
=== 2.1.3 主线：main() 四阶段

系统初始化分为四个主要阶段：

*1. 基础服务初始化*
```cpp
DtbManager::init(dtb_addr);
k_printer.init();
trap_mgr.init();
trap_mgr.inithart();
plic_mgr.init();
plic_mgr.inithart();
intr_stats::k_intr_stats.init();
```

*2. 进程与内存管理初始化*
```cpp
proc::k_pm.init();
mem::k_pmm.init();
mem::k_vmm.init();
mem::k_hmm.init();
shm::k_smm.init();
mem::SlabAllocator::init();
```

*3. 设备与文件系统初始化*
```cpp
k_devm.register_stdin();
k_devm.register_stdout();
k_devm.register_stderr();
tmm::k_tm.init();
syscall::k_syscall_handler.init();
virtio_disk_init();
```

*4. 启动用户进程和调度器*
```cpp
proc::k_pm.user_init();
proc::k_scheduler.init();
proc::k_scheduler.start_schedule();
```



== 2.2　LoongArch 启动流程

=== 2.2.1 入口：entry.S

// 正文待补：QEMU/EDK2 固件将 DTB 物理地址放在 $a1，跳转 _entry；
// move $s0, $a1 保存 DTB 地址；
// 设置 DMWIN0/DMWIN1；
// CSR 初始化：CRMD、PRMD、EUEN；
// 栈分配：sp = boot_stack + 4096 × (CPUID + 1)；
// hartid 从 CSR_CPUID 动态读取；
// tp = CPUID；bl main 直接跳转（LA 没有 start() 薄层）。

=== 2.2.2 main() 四阶段（仅标注 LA 与 RISC-V 的差异）

// 正文待补：仅列出与 RISC-V 的差异点。

==== 阶段一差异

// 正文待补：find_dtb_and_initrd() 暴力搜索 DTB + initrd；
// apic_init() + extioi_init() 两级中断控制器。

==== 阶段二差异

// 正文待补：split heap —— DTB 多段内存时，低端做 buddy，高端整段做 heap+shm。

==== 阶段三差异

// 正文待补：virtio_probe() 先做 PCI 枚举。

==== 阶段四

// 正文待补：和 RISC-V 完全一致。

== 2.3 双架构支持特色

=== 2.3.1 启动完成标志

// 正文待补：双架构启动完成标志的设计与实现。

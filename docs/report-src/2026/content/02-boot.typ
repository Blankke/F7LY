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

LoongArch 将 `start.cc` 阶段合并到 `entry.S` 中，在其中完成设置设备操作空间（DMWIN0）、设置指令数据访问空间（DMWIN1），CSR 寄存器的初始化，以及栈空间的设定，并开启 FPU 浮点数指令。保存 `$a1` 传入的 DTB地址，hartid 使用 CSR_CPUID 动态读取。完成设置后直接 `bl main`。

```cpp
        # entry只对每个CPU设定自己的栈空间，然后跳转到main函数

        li.d        $t0, 0x8000000000000001
        csrwr       $t0, LOONGARCH_CSR_DMWIN0   # 设置设备操作空间
        li.d        $t0, (0x9000000000000001 | 1 << 4)
        csrwr       $t0, LOONGARCH_CSR_DMWIN1   # 设置指令数据访问空间
        csrwr       $t0, LOONGARCH_CSR_TLBRENTRY   # 关闭TLB，其他两个窗口不用管

        li.w	    $t0, 0xb0		      # PLV=0, IE=0, PG=1
	csrwr	    $t0, LOONGARCH_CSR_CRMD
        li.w        $t0, 0x00                 # PPLV=0, PIE=0, PWE=0
        csrwr       $t0, LOONGARCH_CSR_PRMD
        li.w        $t0, 0x01                 # FPE=1, SXE=0, ASXE=0, BTE=0
        csrwr       $t0, LOONGARCH_CSR_EUEN
```

=== 2.2.2 main() 四阶段

系统初始化同样分为四个阶段，仅以下三处与 RISC-V 不同：

- 阶段一：`find_dtb_and_initrd()` 暴力搜索 DTB 与 initrd（RISC-V 由
  OpenSBI 传入，无需搜索）；中断控制器为 `apic_init()` + `extioi_init()`
  两级架构（RISC-V 为 PLIC 单级）。
- 阶段二：split heap —— DTB 包含多段内存时，低端区域做 buddy，高端区
  域整段做 heap + shm。
- 阶段三：`virtio_probe()` 先进行 PCI 总线枚举（RISC-V 走 MMIO 不需要）。
- 阶段四：与 RISC-V 一致。

```cpp
extern "C" void main(uint64 hartid, uint64 dtb_addr)
{
    k_printer.init();
    printfYellow("Hello, World!\n");

    // Initialize DTB and scan Initrd if necessary
    uint64 kernel_end_phys = ((uint64)end) & 0x0FFFFFFFFFFFFFFFUL;
    DtbManager::find_dtb_and_initrd(dtb_addr, kernel_end_phys);
    
    printfMagenta("[main] Using hartid=%lu, k_dtb_addr=0x%lx\n", hartid, k_dtb_addr);

    apic_init();
    extioi_init();

    // ... 与 RISC-V 阶段一至阶段三相同的初始化 ...
    virtio_probe();            
    virtio_disk_init();       
    // ...
```
== 2.3 双架构支持特色

- *统一的抽象层*：通过HAL(硬件抽象层)实现跨架构代码复用。
- *架构特定优化*：针对不同架构的特性进行专门优化。
- *模块化设计*：核心模块与架构相关代码分离，便于维护和扩展。
- *现代化实现*：采用C++面向对象设计，提供类型安全和更好的代码组织。

=== 2.3.1 启动完成标志

```cpp
╦ ╦╔═╗╦  ╔═╗╔═╗╔╦╗╔═╗
║║║║╣ ║  ║  ║ ║║║║║╣
╚╩╝╚═╝╩═╝╚═╝╚═╝╩ ╩╚═╝

=== SYSTEM BOOT COMPLETE ===
Kernel space successfully initialized
```

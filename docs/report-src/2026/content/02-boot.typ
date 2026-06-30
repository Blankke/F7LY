= 机器启动

F7LY-OS 支持 RISC-V 与 LoongArch 两条启动链路。两种架构共享统一的内核入口约定，但在固件交接方式、设备发现和中断控制器初始化等方面各自遵循本架构的硬件模型。

整个 kernel 可执行文件的入口点统一为 `_entry`，由链接脚本指定到对应架构的早期汇编入口。汇编入口只负责建立最小可运行环境，随后尽快进入 C++ 主初始化流程。

*RISC-V 架构*：
启动流程：`OpenSBI`（M-mode）→ `entry.S` → `start.cc` → `main()`。基于SBI（Supervisor Binary Interface）规范，Bootloader 工作在 M-mode，内核代码从 `entry.S` 开始运行在 S-mode。

*LoongArch 架构*：
启动流程：`QEMU/EDK2` → `entry.S` → `main()`（无 `start.cc` 薄层）。采用LoongArch 原生启动方式，通过 DTB 进行硬件发现与初始化。

== RISC-V 启动流程

=== entry.S

RISC-V 入口阶段只做三件事：根据 hartid 选择每个 CPU 独立的早期栈，保留固件传入的设备树地址，然后跳转到 `start()`。这一层不做复杂初始化，目的是尽快离开汇编环境，避免架构细节扩散到主流程。

=== start.cc

`start.cc` 是 RISC-V 在启动链中为过渡作用。它关闭早期分页、安装临时 trap 兜底入口、记录当前 hartid，并把设备树地址交给 `main()`。从这一点开始，RISC-V 与 LoongArch 尽量共享同一套内核初始化顺序。

=== 主线：main() 四阶段

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

*3. 初始进程、设备与文件系统初始化*
```cpp
k_devm.register_stdin();
k_devm.register_stdout();
k_devm.register_stderr();
tmm::k_tm.init();
syscall::k_syscall_handler.init();
proc::k_pm.user_init();
virtio_disk_init();
init_fs_table();
binit();
fileinit();
inodeinit();
fs::k_file_table.init();
vfs_ext4_init();
fs::k_vfs.dir_init();
fs::k_fifo_manager.init();
dev::LoopControlDevice::init_loop_control();
```

*4. 启动调度器*
```cpp
proc::k_scheduler.init();
proc::k_scheduler.start_schedule();
```



== LoongArch 启动流程

=== 入口：entry.S

LoongArch 将 `start.cc` 阶段合并到 `entry.S` 中，在其中完成设置设备操作空间（DMWIN0）、设置指令数据访问空间（DMWIN1），CSR 寄存器的初始化，以及栈空间的设定，并开启 FPU 浮点数指令。保存 `$a1` 传入的 DTB地址，hartid 使用 CSR_CPUID 动态读取。完成设置后直接 `bl main`。

=== main() 四阶段

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
== 双架构支持特色

- *统一的抽象层*：通过HAL(硬件抽象层)实现跨架构代码复用。
- *架构特定优化*：针对不同架构的特性进行专门优化。
- *模块化设计*：核心模块与架构相关代码分离，便于维护和扩展。
- *现代化实现*：采用C++面向对象设计，提供类型安全和更好的代码组织。

=== 启动完成标志

```cpp
╦ ╦╔═╗╦  ╔═╗╔═╗╔╦╗╔═╗
║║║║╣ ║  ║  ║ ║║║║║╣
╚╩╝╚═╝╩═╝╚═╝╚═╝╩ ╩╚═╝

=== SYSTEM BOOT COMPLETE ===
Kernel space successfully initialized
```

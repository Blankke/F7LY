#set par(first-line-indent: 2em)

= 内核详细介绍

== 机器启动

F7LY OS支持双架构启动，机器启动的源文件分别在 `kernel/boot/riscv/` 和 `kernel/boot/loongarch/` 目录下。

启动流程遵循经典的多阶段启动模式：`Bootloader -> entry.S -> start.cc` ` -> main.cc`

整个`kernel`可执行文件的入口点在各自架构的`entry.S`中,通过链接脚本指定。

*RISC-V架构*：基于SBI(Supervisor Binary Interface)规范，Bootloader工作在M-mode，内核代码从entry.S开始运行在S-mode。

*LoongArch架构*：采用LoongArch原生启动方式，通过ACPI等标准接口进行硬件初始化。

=== RISC-V启动流程

==== entry.S阶段

- 负责设置操作系统的栈指针`sp`到合适位置，为后续C++代码执行做准备。
- 每个CPU核心分配4KB独立栈空间，通过`hartid`进行区分。
- 完成栈设置后跳转到`start()`函数。

```c
_entry:
    la sp, stack0           # Load base addr of stack
    li t0, 1024*4          # 4KB space per stack
    mv t1, a0              # gain hartid
    addi t1, t1, 1
    mul t0, t0, t1         # cal current offset of stack
    add sp, sp, t0         # set stack pointer
    call start             # jump to start func
```

==== start.cc阶段

关键寄存器处理：
- `a0`：存储硬件线程编号`hartid`。
- `a1`：设备树地址信息dtb_entry  。
- `sp`：已在entry.S中设置完成。

主要工作：
- 关闭分页机制，使用物理地址访问
- 设置临时trap处理函数为死循环
- 将hartid保存到tp寄存器供后续使用
- 调用main()函数进行系统初始化

```cpp
void start(uint64 hartid, uint64 dtb_entry)
{
    riscv::csr::_write_csr_(riscv::csr::satp, 0);
    riscv::csr::_write_csr_(riscv::csr::stvec, (uint64)trap_loop);    
    riscv::w_tp(hartid);    // Save hartid
    main();
}
```

==== 主函数初始化

系统初始化分为三个主要阶段：

*1. 基础服务初始化*
```cpp
k_printer.init();              
trap_mgr.init();               
plic_mgr.init();               
```

*2. 内存管理初始化*
```cpp
mem::k_pmm.init();             
mem::k_vmm.init();             
mem::k_hmm.init();            
```

*3. 进程和设备管理*
```cpp
proc::k_pm.init();
dev::k_devm.register_stdin();
riscv::qemu::disk_driver.init();
syscall::k_syscall_handler.init(); 
```

*4. 启动用户进程和调度器*
```cpp
proc::k_pm.user_init(); 
proc::k_scheduler.start_schedule(); 
```

=== LoongArch启动流程

loongarch部分将`start.cc`阶段合并到`entry.S`中，在其中完成设置设备操作空间、设置指令数据访问空间，CSR寄存器的初始化，以及栈空间的设定，并开启fpu浮点数指令。

```asm
# entry只对每个CPU设定自己的栈空间，然后跳转到main函数

li.d        $t0, 0x8000000000000001
csrwr       $t0, LOONGARCH_CSR_DMWIN0   # 设置设备操作空间
li.d        $t0, (0x9000000000000001 | 1 << 4)
csrwr       $t0, LOONGARCH_CSR_DMWIN1   # 设置指令数据访问空间
csrwr       $t0, LOONGARCH_CSR_TLBRENTRY 

li.w	    $t0, 0xb0		            # PLV=0, IE=0, PG=1
csrwr	    $t0, LOONGARCH_CSR_CRMD
li.w        $t0, 0x00                   # PPLV=0, PIE=0, PWE=0
csrwr       $t0, LOONGARCH_CSR_PRMD
li.w        $t0, 0x01                   # FPE=1, SXE=0, ASXE=0, BTE=0
csrwr       $t0, LOONGARCH_CSR_EUEN
```

其余大部分启动的基本流程与riscv类似，在中断与虚拟化方面LoongArch架构采用不同的启动方式：
- 使用ACPI控制器进行硬件发现和初始化。
- 通过APIC和ExtiOI进行中断管理。
- 支持LoongArch原生的虚拟化和安全特性。

```cpp
void main()
{
    k_printer.init();
    dev::acpi::k_acpi_controller.init(); 
    apic_init();  
    extioi_init(); 
    // ... 后续与RISC-V类似的初始化流程
}
```

=== 双架构支持特色

- *统一的抽象层*：通过HAL(硬件抽象层)实现跨架构代码复用。
- *架构特定优化*：针对不同架构的特性进行专门优化。
- *模块化设计*：核心模块与架构相关代码分离，便于维护和扩展。
- *现代化实现*：采用C++面向对象设计，提供类型安全和更好的代码组织。

==== 启动完成标志

系统启动完成后会显示欢迎信息并启动调度器：

```
=== SYSTEM BOOT COMPLETE ===
Kernel space successfully initialized
```

此时内核已完成所有初始化工作，进入正常的多进程调度运行状态。

== 中断管理器

从用户态到内核态的切换需要中断和异常的频繁处理，此处F7LY在迁移xv6代码的过程中选择了对象化中断管理器类TrapManager，并实例化全局对象trap_mgr。

```cpp
class trap_manager
{
public:
    void init();
    void inithart();
    int devintr();      // 处理外部中断和软件中断
    void usertrap();    
    void usertrapret(); 
    void machine_trap();
    void kerneltrap();  
private:
    void timertick();  
    SpinLock tickslock; 
    uint ticks;         
    uint timeslice;    
};
```

F7LY在其底层根据RISC-V和loongarch的不同硬件设备使用而封装了不同的中断处理器，在不同文件夹下实现了PLIC、EXTIOI、APIC的驱动并应用在中断处理之中。

- *RISC-V*在处理中设定了中断处理的入口（`uservec`）以及返回的入口（`user-` `trapret`），具体逻辑参照了xv6的中断处理，但封装在对象之中，并使用包装函数（`wrap_`前缀的全局函数）设置中断入口。
- *Loongarch*针对架构的特殊性设置了TLB重填的处理程序入口（`tlbrefill.S`），以及机器异常的处理程序（`merror.S`），使用类似的逻辑进行接口统一化，使得上层程序可以统一调用。

如果用户程序发出系统调用，或者做了一些非法的事情，或者设备中断，那么在用户空间中执行时就可能会产生陷阱。来自用户空间的陷阱的高级路径是`uservec`，然后是`usertrap`；返回时，先是`usertrapret`，然后是`userret`。

首先通过`crmd`寄存器确认陷阱确实来自用户模式，然后将中断入口点（`eentry`）设为`kernelvec`，并获取当前进程信息并保存用户程序计数器。

函数通过检查异常代码（CSR_ESTAT_ECODE）来区分不同类型的陷阱：

- *系统调用* (0xb)：
  - 检查进程是否被标记为结束。
  - 调整返回地址，使其指向 ecall 指令之后的指令。
  - 启用中断，然后调用系统调用处理器。
- *内存错误* (0x1 或 0x2)：
  - 调用 mmap_handler来处理缺页异常。
  - 如果处理失败，则标记进程为已终止。
- *设备中断*：
  - 调用 `devintr()`处理设备中断。
- *未知陷阱*：
  - 打印错误信息并标记进程为已终止。

如果是时钟中断（`which_dev == 2`），则增加时间片计数，当达到阈值（10个时间片）时调用`yield`。最后调用 `usertrapret()` 函数返回用户态，该函数负责恢复用户态执行环境。

== 内存管理

=== 地址空间

==== 内核链接地址

F7LY 在支持多架构内核时，并未采用"一体适配"的简化策略，而是根据架构自身特性量身定制内核布局，以保障启动路径的最小依赖性和最大清晰性。

对于RISC-V，qemu的入口地址位于`0x8020_0000`。在此处，F7LY采用OPENSBI自动识别内核的ELF文件入口地址（LMA），低地址采用直接映射的方式映射所有MMIO设备以及内核代码。因为在进入到`entry.S`时仍然未开启分页，所以映射时低处地址采用直接映射便于访问。

与RISC-V使用 QEMU 默认跳转地址（`0x80200000`）不同，LoongArch 架构的启动顺序允许我们将内核加载地址与运行地址（VMA = LMA）分离，从而更灵活地构造分页映射策略。F7LY的LoongArch端应用loongarch的直接映射窗口，在链接脚本处使用`0x9000000080000000`加载内核镜像。

#figure(
  image("fig/决赛内核地址空间.png", width: 90%),
  caption: [内核地址空间布局],
) <fig:address-layout>

==== 地址空间布局

F7LY的地址空间的设计参考了xv6的物理地址布局，内核态页表保存在全部内核地址空间Kernel中，其记录了内存的空间布局。用户页表在用户空间中记录用户态地址。

对于内核地址空间，为方便管理物理地址，从KERNBASE开始映射内核映像；堆内存从地址`0x840000000`开始，剩余的空闲物理地址交由物理内存管理器管理，用于后续的动态分配。

对于用户地址空间，为便于堆空间的增长和用户程序的动态访问，将用户地址的代码从`Proc_base`进行加载。每当用户对内存进行申请时，采用对`p->_heaptop`指针的移动进行扩张。由于loongarch的加载地址与riscv不一致，我们在execve对程序进行加载时显式设置程序的空间开始地址（`PROC_BASE`），并从此处开始设置`p->_heaptop`指针，装载完成后的指针位置既标志了进程此时的大小，又可作为堆顶指针开始动态内存分配。

#figure(
  image("fig/决赛用户地址空间.png", width: 90%),
  caption: [用户地址空间布局],
) <fig:user-address-layout>

=== 物理内存管理

==== 内核动态内存分配器

F7LY使用粗细粒度混合分配管理内核所需的物理内存，统一使用伙伴内存分配器进行粗粒度分配。伙伴分配器用于管理一片连续内存，从一个地址开始初始化，并设置最多支持的分配粒度便可以指定伙伴分配器管理的一片连续内存。

```cpp
#define PAGE_ORDER 10
#define PGNUM (1 << 15)
#define BSSIZE 10 //最多支持 2^10 = 1024 页的分配粒度

class BuddySystem {
public:
    int Alloc(int size);        //按照大小分配
    void Free(int offset);
    void* alloc_pages(int count);   //按照页数分配
    void free_pages(void* ptr);
private:
    uint8* tree;        //建立对一片内存管理的树结构
    uint8* base_ptr;    //管理内存的起始地址
}
```

伙伴分配器(Buddy Allocator)通过分配和管理内存块来满足不同大小的内存请求，并进行高效的合并和分割操作。其优点在于分配和释放内存块的操作非常快速，且通过内存块大小的选择和合并操作，有效减少了外部碎片。但是对于页面内部的空间碎片，需要更细粒度的管理器进行分配，考虑到空闲物理空间与内核堆空间应用场景的不同，我们选择使用不同的细粒度分配器进行管理。

==== 内核物理内存

内核中的物理空间，即实际分配的物理页面是在操作系统中应用最多的空间，F7LY使用对象化资源管理的思想，设计物理管理类`PhysicalMemoryManager`并创建全局对象`k_pmm`进行管理。

```cpp
class PhysicalMemoryManager
{
public:
    static void *alloc_page();      
    static void free_page(void *pa);
    static void *kmalloc(size_t size);
    static void *kcalloc(uint n, size_t size);
    void clear_page(void *pa);
private:
    static BuddySystem *_buddy;
    static uint64 pa_start;
    // ......
};
extern PhysicalMemoryManager k_pmm;
```

`k_pmm`在初始化时会使用地址布局中的地址标志(`pa_start`)，原本Buddy的初始化放在这里，Buddy变成pmm的一个成员`pa_start`是buddy系统在物理内存中的起始地址，加上一个`Sizeof(BuddySystem)`后后面存的东西是tree，然后tree存完了之后才是buddy系统管理的那块内存。加上的BSSIZE是预留来放BuddySystem的大小和tree的大小，这之后才是buddy系统管理的那块内存，这时`pa_start`指向的就是buddy系统管理的那块内存的开始地址，再被初始化为buddy的基址。

```cpp
void PhysicalMemoryManager::init()
{
    memlock.init("memlock");
    pa_start = reinterpret_cast<uint64_t>(end);
    pa_start = (pa_start + PGSIZE - 1) & ~(PGSIZE - 1); 
    _buddy = reinterpret_cast<BuddySystem*>(pa_start);
    pa_start += BSSIZE * PGSIZE;
    memset(_buddy, 0, BSSIZE * PGSIZE);
    _buddy->Initialize(pa_start);
}
```

对于更细粒度的大小分配（函数`kmalloc`），F7LY采用了linux的`SlabAllocator`。slab allocator背后的思想是缓存经常使用的object并保持在初始状态供kernel使用。如果被基于object的allocator，内核将耗费很多时间在分配，初始化和释放相同的object。slab allocator的目的就是缓存一些被释放的object因此这些基础的structures在多次调用期间被预留起来。

#figure(
  image("fig/物理内存管理.png", width: 80%),
  caption: [物理内存管理],
) <fig:physical-memory-management>

slab allocator由一组cache组成，这些cache由一个叫做cache chain的双向循环链表连接在一起。在slab allocator的上下文件，一个cache就是一个管理许多像`mm_struct`或者`fs_cache`这种特殊类型的object的管理者。这些cache通过cache struct的next字段连接在一起。

每一cache维护了有多个连续物理page组成的block，这些block称之为slab。slab被切分成很多小块来存放slab自身的数据结构和其管理的object。

```cpp
class SlabCache
{
private:
    static constexpr uint32 DEFAULT_MAX_FREE_SLABS_ALLOWED = 5;
    uint32 obj_size_;
    uint32 free_slabs_count_;
    LinkedList<Slab> free_slabs_;    
    LinkedList<Slab> partial_slabs_; 
    LinkedList<Slab> full_slabs_;   
    ......
}
```

Slab与Buddy二者分层次调用即可实现细粒度的物理内存分配，更小块的内存分配可以帮忙消除buddy allocator原本会造成的内部碎片问题。

#figure(
  image("fig/堆空间管理.png", width: 80%),
  caption: [堆空间管理],
) <fig:heap-layout>

==== 内核堆内存分配

与物理内存类似，堆内存是cpp中用`new`动态创建对象的重要方法，为在内核环境中实现`new`与`delete`的正确功能，我们在内核中开辟一块堆空间，并重载new与delete来完成我们自己的对象实例化。

```cpp
class HeapMemoryManager
{
private:
    SpinLock _lock;
    BuddySystem* _k_allocator_coarse;   
    L_Allocator _k_allocator_fine;      
public:
    HeapMemoryManager() {};
    void init(const char *lock_name, uint64_t heap_start); 
    void *allocate(uint64 size);
    void free(void *p);
};
extern HeapMemoryManager k_hmm;
```

对于堆，设置管理类`HeapMemoryManager`并创建全局对象`k_hmm`进行管理。`k_hmm`在初始化时会从堆地址开始初始化Buddy System，并将此处划分为堆空间。

为提升堆分配的性能、减少碎片，我们在堆空间上引入了第三方高效内存分配器LibAllocator#footnote[https://github.com/blanham/liballoc]，主要用于进行高效内存管理。分配器设计为二层结构：上层是*粗粒度分配器（`L'Major`）*，负责大块内存获取；下层是*细粒度分配器（`L'Minor`）*，用于将大块切分成适用于常规 `new`/`malloc` 调用的空间，极大提升了小对象分配的效率。

F7LY 对 LibAllocator 进行了定制性适配，使其不再从物理内存中直接申请页帧，而是通过内核内部的 BuddyAllocator 分配大块内存作为 `L'Major` 的来源。这种结构层次分明、责任明确，既保持了 Buddy 分配器的可控性，也引入了 LibAllocator 在细粒度分配上的高效策略。

#figure(
  image("fig/buddy-allocator.png", width: 90%),
  caption: [Buddy Allocator 分配示意图],
) <fig:buddy-allocator>

F7LY 重载了标准的 `new` / `delete` 运算符，使其默认在堆空间上分配内存并交由 `k_hmm` 管理：

```cpp
void * operator new(uint64 size)
{
    void *p = mem::k_hmm.allocate(size);
    return p;
}

void operator delete(void * p) noexcept
{
    mem::k_hmm.free(p);
}      
// 省略展示其余用法的new和delete运算符。
```

通过这种方式，内核中的对象创建和销毁行为得以统一，所有堆内分配操作均通过 `k_hmm` 路由，有效避免了裸指针操作和分配器混用的问题。

==== 地址空间管理

F7LY采用*静态预配置+管理器分派*的机制进行地址空间管理。核心由 `VirtualMemoryManager` 管理器统一负责虚拟地址的分配、映射与回收，底层则由 `PageTable` 类协助完成页表操作与维护。这种结构设计不仅提升了内存管理的确定性和效率，也使得地址空间的生命周期管理更为清晰。

针对 `mmap` 所涉及的文件映射与匿名映射，F7LY在每个进程控制块（PCB）中*预先分配了一块VMA表区域*，用于记录与管理该进程的虚拟内存区域（VMA），避免了运行时动态结构分配所带来的复杂性与不确定性。

*页表*

PageTable 类用于抽象和管理多级页表结构。其核心成员变量包括：

- `base_addr`页表的物理基地址，指向页表的起始位置。
- `_is_global`标记该页表是否为全局页表（如内核页表）。

主要接口包括：

- `walk(uint64 va, bool alloc)`：软件递归遍历多级页表，查找或分配虚拟地址 va 对应的 PTE。可用于递归查找虚拟地址的映射关系，必要时分配中间页表。
- `walk_addr(uint64 va)`：返回虚拟地址 va 映射的物理地址指针。
- `freewalk()` / `freewalk_mapped()`：递归释放页表及其映射的物理页。
- `get_pte(index)` / `set_pte(index, pte)`：获取/设置指定索引的 PTE。

在walk函数中，由于RISC-V使用SV39标准页表，而loongarch使用4级页表，二者不可统一，在此处F7LY分别定义了不同的实现，并在编译时根据宏进行区别。

*PTE*

PTE类封装了单个页表项（Page Table Entry），通常包含如下信息：

- 物理页帧号（PPN）。
- 有效位（Valid）、读/写/执行权限（R/W/X）、用户/超级用户位（U/S）等标志位。

上述信息由于loongarch和RISC-V的架构不同而设置了不同的访问接口，可用于映射时设置或访问时查询。

*虚拟内存管理器*

VirtualMemoryManager是内核虚拟内存的管理器，同样设置了全局对象`k_vmm`进行内存管理，它为进程和内核提供统一的虚拟地址空间分配、映射、回收、拷贝等功能，屏蔽了底层不同架构（如 RISC-V、LoongArch）页表实现的差异。

主要功能如下：

- *虚拟地址到物理地址的映射与解除映射*：通过 `map_pages`、`vmunmap`、`kvmmap` 等接口，将虚拟地址空间映射到物理内存，或解除映射关系。
- *内核页表的创建与初始化*：通过 `kvmmake` 创建内核页表，`kvmmap` 完成内核空间的初始映射。
- *用户空间内存分配与回收*：通过 `uvmalloc`、`uvmdealloc` 等接口，支持进程虚拟空间的动态增长与收缩。
- *内存拷贝与字符串拷贝*：提供 `copy_in`、`copy_out`、`copy_str_in` 等接口，实现用户态与内核态之间的数据安全拷贝。

通过条件编译（如 `#ifdef` RISCV）自动适配不同架构下的页表实现，保证接口一致性与可移植性。

*文件映射与匿名映射*

针对mmap系统调用的匿名映射和文件映射，F7LY采用在进程控制块中预留一片区域的方法进行管理。

```cpp
struct vma
{
    int used;        
    uint64 addr;     
    int len;         
    int prot;       
    int flags;       
    int vfd;        
    fs::normal_file *vfile;
    int offset;     
    uint64 max_len;  
    bool is_expandable; 
};
```

当用户态调用 `mmap` 系统调用时，内核会查找该进程 PCB 中的空闲 VMA 槽位，将映射信息记录其中，并返回分配好的虚拟地址。若为文件映射，则填入对应的 `vfile` 和 `offset` 字段；若为匿名映射，则标记 `flags` 中的 `MAP_ANONYMOUS`，并将 `vfile` 设为 `nullptr`。

==== 缺页异常处理

F7LY目前能够利用缺页异常处理来实现写时复制（Copy on write）、地址空间的懒分配（Lazy page allocation）以及用户的地址检查机制。

#figure(
  image("fig/缺页.png", width: 60%),
  caption: [缺页异常处理流程],
) <fig:page-fault>

当用户程序因缺页异常进入内核时，两个架构的异常处理程序使用同样的处理逻辑，先检查缺页的地址是否处于物理空间，或处于vma记录的地址空间内，若是，则分配物理页面并建立映射。若不是，则抛出缺页错误。

== 进程管理

=== 进程控制块(PCB)

进程是操作系统中资源分配的基本单位。每个进程都有自己独立的地址空间和资源，如内存、文件描述符等。线程是操作系统中CPU调度的基本单位，线程共享所在的进程的地址空间和资源，但是有独立的执行上下文。

许多操作系统内核的设计都将进程和线程分开设计，分别使用Process和Thread结构体来表示。但其实在Linux内核中并没有严格地区分进程和线程，而是通过一组统一的API来操作任务。例如`sys_clone`通过不同的flags组合创建共享不同资源的新任务，因此进程和线程的创建本质上是类似的。F7LY此处统一使用Pcb结构体，并在Pcb中设置globa_id、pid、tid、ppid、pgid等标准Linux进程标识符用于标记进程和线程状态，线程的共享内存状况也一并记录便于管理。

==== PCB的组成

```cpp
class Pcb;
extern Pcb k_proc_pool[num_process];
```

详细字段放于文末附录进行介绍。

==== 进程的状态

在所有的使用之中，进程需要的状态可简略分为5种，含义如下：

#table(
  columns: (auto, 1fr),
  align: left,
  stroke: 0.5pt,
  [*状态*], [*含义*],
  [RUNNING], [正在运行或准备运行的任务。此状态下，任务占用CPU，执行其代码],
  [RUNNABLE], [进程处于就绪队列中，当正在运行的进程放弃CPU或时间片耗尽后会按照队列获取CPU],
  [UNUSED], [进程创建而来的状态，即初始状态],
  [USED], [进程分配时进行切换，在创建和就绪之间的状态],
  [SLEEPING], [时间片耗尽或等待某个事件。该状态下进行可以被信号唤醒],
  [ZOMBIE], [进程已终止，但PCB依然存在，以便父进程读取退出状态并等待回收],
)

进程状态的切换如下：

- 创建进程池时构造函数中赋值UNUSED
- `alloc_proc()`从空闲进程中初始化时创建用户页表分配物理空间，并切换为USED。
- `user_init()`中初始化进程，并在所有初始化完成后切换为RUNNABLE。
- 调度器从就绪队列中找到优先级最高的进程，开始运行，并将状态设为RUNNING。
- 当被系统调用终止或时间片用尽时进程放弃CPU，状态回到RUNNABLE。
- 当futex或sleep系统调用让进程进入休眠时，进程切换为SLEEPING，此时等待信号唤醒进程。
- 进程使用`exit`退出时状态切换为ZOMBIE。

=== 进程调度

为便于全局调用，F7LY使用调度器Scheduler类以及全局对象`k_scheduler`进行进程调度管理，其中具体的调度方法使用了与MIT xv6类似的逻辑方法。

```cpp
class Scheduler
{
private:
    SpinLock _sche_lock;
public:
    Scheduler() = default;
    void init(const char *name);    
    void switch_to_proc(Pcb* p);    // 调度切换
    int  get_highest_proirity();   
    void start_schedule();          

    void yield();                   // 进程放弃CPU，状态从RUNNING改为RUNNABLE
    void call_sched();             
};
```

==== 上下文切换

汇编函数`swtch.S`用于管理一个到旧进程内核线程的用户-内核转换（系统调用或中断），一个到当前CPU调度程序线程的上下文切换，一个到新进程内核线程的上下文切换，以及一个返回到用户级进程的陷阱。

#figure(
  image("fig/上下文切换.png", width: 70%),
  caption: [上下文切换],
) <fig:context-switch>

`swtch`对线程没有直接的了解；它只是保存和恢复上下文（Contexts），`call_sched`调用`swtch`切换到`cpu->scheduler`，即每个CPU的调度程序上下文。调度程序上下文之前通过`scheduler`对`swtch`（`swtch(&p->_context, cpu` `->get_context());`）的调用进行了保存。当我们追踪`swtch`到返回时，它返回到`scheduler`而不是`sched`。

==== 有栈协程调度

Scheduler类承担了原本c语言书写的xv6中的调度任务，存在一种情况使得调度程序对`swtch`的调用没有以`sched`结束。一个新进程第一次被调度时，它从`forkret`开始。`Forkret`存在以释放`p->lock`；否则，新进程可以从`usertrapret`开始。

`Scheduler`（运行一个简单的循环：根据优先级找到要运行的进程，运行它直到它让步，然后重复循环。`scheduler`在进程表上循环查找可运行的进程，比较它们的优先级并选出最高优先级，该进程具有`p->state == RUNNABLE`。一旦找到一个进程，它将设置CPU当前进程变量`c->proc`，将该进程标记为`RUNINING`，然后调用`swtch`开始运行它。

这种用对象封装两个有栈协程`call_sched`和`start_schedule`的方法可以使F7LY比xv6有更清晰和高效的调度方法。

=== 进程内存管理器

在F7LY的设计中，考虑到系统调用中涉及大量与进程相关的操作，我们避免将所有下层接口封装到一个单一的管理器类中，以防止类的臃肿，并保持面向对象设计的简洁性与高效性。因此，我们采用了*双管理器*的设计模式来实现进程管理与内存管理的分离。

具体来说，*进程内存管理器*（`ProcessMemoryManager`）是专门用于管理进程的内存操作的类。它封装了许多与进程内存增长、共享内存引用计数等相关的操作，并为内存相关的系统调用提供了清晰的接口。通过这种方式，内存管理的逻辑更加集中，避免了冗余代码，并且提高了代码的可维护性。

在实现上，*进程内存管理器*类不使用全局管理对象，而是为每个进程单独创建一个实例，确保每个进程的内存管理是独立且高效的。这样不仅符合面向对象设计原则，还能确保进程的内存操作彼此隔离，避免了潜在的资源冲突。

```cpp
class ProcessMemoryManager
    {
    public:
        // 程序段管理
        program_section_desc prog_sections[max_program_section_num];
        int prog_section_count;
        // 堆内存管理
        uint64 heap_start;
        uint64 heap_end;
        // 页表管理
        mem::PageTable pagetable;
        // VMA管理
        VMA vma_data;
        // 共享标志
        bool shared_vm;
    private:
        // 内存大小
        uint64 total_memory_size;
    public:
        //...各种公共方法
    }
```

==== 线程的创建与释放

在F7LY内核中，进程使用独立的内存管理器来支持线程的创建和释放。虽然线程拥有独立的上下文，但它们位于同一进程中，且共享进程的地址空间。这种设计使得线程间可以高效共享内存，同时确保每个线程的上下文是独立的。为了支持线程的共享内存，我们使用了`ProcessMemoryManager` 类的多种方法。具体而言，以下方法在F7LY中用于支持线程的共享内存操作：

```cpp
       ProcessMemoryManager *share_for_thread(); //为线程创建共享内存（增加引用计数）
        ProcessMemoryManager *clone_for_fork(); //为进程创建完全复制的内存管理器
```

线程的创建与释放流程，尤其在`fork`和`clone`系统调用中，涉及以下步骤：

1. *分配与初始化子Pcb*：在调用`fork`和`clone`时，首先会为子进程分配并初始化一个新的进程控制块（Pcb）。    
2. *复制/共享打开文件*：在新创建的Pcb中，子进程会继承父进程的文件描述符，这些描述符指向相同的文件对象。 
3. *选择内存策略*：根据`clone`调用传入的标志位（如`CLONE_VM`），决定是否共享内存。
   - 不共享内存:如果不共享内存，子进程会创建一个完全独立的内存管理器。此时调用父进程的`clone_for_fork`方法，创建一个深度拷贝的内存管理器。在此过程中，系统为子进程分配一个全新的页表，并将`trampoline`、`sig_trampoline`以及程序段描述数组（`prog_sections`）与计数、堆元数据、VMA数据等内容进行复制。最终，复制后的进程内存管理器对象会与新进程关联。 
   - 共享内存:如果共享内存，子进程会调用父进程的`share_for_thread`方法，创建一个指向同一内存管理器的引用，并增加引用计数。这样，子进程和父进程将共享同一块内存区域，从而实现高效的内存使用。
4. *完成初始化与返回*：在完成进程的内存管理器设置后，继续进行其余的进程初始化工作，最终返回子Pcb对象。

==== 内存增长与回收

F7LY通过`ProcessMemoryManager`类提供了一系列方法来支持进程内存的动态增长与回收。这些方法涵盖了从堆内存的扩展与收缩，到虚拟内存区域（VMA）的管理等多个方面。通过这些接口，F7LY可以支持sbrk、brk、mmap系统调用为进程提供动态内存分配的支持。

此外，系统通过 `/proc/stat`路径暴露进程的内存占用状态，方便外部工具或系统管理员查看进程的内存使用情况。

==== 内存释放

在F7LY中，进程内存的管理采用了统一的资源释放机制，以确保内存资源能够安全、准确地释放。我们通过`free_all_memory`方法来释放进程的所有内存资源，并且在内存资源的释放过程中，确保线程间的共享内存不会发生悬挂引用或重复释放的问题。

该方法的工作流程如下：
1. *检查共享标志*：首先，方法会检查进程内存管理器中的`shared_vm`标志，以确定当前进程是否与其他线程共享内存。
2. *处理共享内存*：在释放内存前，首先会减少共享内存的引用计数。只有当引用计数降为0时，内存资源才会被实际释放。这样确保了共享内存仅在最后一个引用被释放时才清理掉，从而避免了早期释放导致的悬挂引用问题。 
3. *释放内存资源*：如果当前进程不共享内存，或者引用计数已经降为0，方法会继续释放所有与进程相关的内存资源，包括页表、堆内存、VMA等。通过调用`pagetable.freewalk_mapped()`来递归释放页表及其映射的物理页，并清理其他内存区域。

=== 进程管理器

F7LY的进程管理使用类ProcessManager封装对进程的许多操作，并使用全局对象`k_pmm`进行管理，既包含了对于当前进程的状态信息获取和修改接口，又作为系统调用和进程结构体这一个体之间的桥梁便于通过系统调用直接对进程进行操作。

下面将介绍这一类的公共接口与各字段的含义：

```cpp
class ProcessManager
{
private:
    SpinLock _pid_lock;        
    SpinLock _wait_lock;       
    int _cur_pid;             
    Pcb *_init_proc;           
    uint _last_alloc_proc_gid; 

public:
    ProcessManager() = default;
```

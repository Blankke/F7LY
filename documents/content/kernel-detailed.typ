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
    la sp, stack0          # Load base addr of stack
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

#text()[#h(2em)]主要工作：
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

系统初始化分为四个主要阶段：

*1. 基础服务初始化*
```cpp
k_printer.init();              
trap_mgr.init();               
plic_mgr.init();               
```

#text()[#h(2em)]*2. 内存管理初始化*
```cpp
mem::k_pmm.init();             
mem::k_vmm.init();             
mem::k_hmm.init();            
```

#text()[#h(2em)]*3. 进程和设备管理初始化*
```cpp
proc::k_pm.init();
dev::k_devm.register_stdin();
riscv::qemu::disk_driver.init();
syscall::k_syscall_handler.init(); 
```

#text()[#h(2em)]*4. 启动用户进程和调度器*
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

#text()[#h(2em)]其余大部分启动的基本流程与riscv类似，在中断与虚拟化方面LoongArch架构采用不同的启动方式：
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

#text()[#h(2em)]此时内核已完成所有初始化工作，进入正常的多进程调度运行状态。

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

#text()[#h(2em)]F7LY在其底层根据RISC-V和loongarch的不同硬件设备使用而封装了不同的中断处理器，在不同文件夹下实现了PLIC、EXTIOI、APIC的驱动并应用在中断处理之中。

- *RISC-V*实现了中断处理的入口（`uservec`）以及返回的入口（`user-trapret`），具体逻辑参照了xv6的中断处理，但封装在对象之中，并使用包装函数（`wrap_`前缀的全局函数）设置中断入口。
- *Loongarch*针对架构的特殊性设置了TLB重填的处理程序入口（`tlbrefill.S`），以及机器异常的处理程序（`merror.S`），使用类似的逻辑进行接口统一化，使得上层程序可以统一调用。

#text()[#h(2em)]如果用户程序发出系统调用，或者做了一些非法的事情，或者设备中断，那么在用户空间中执行时就可能会产生陷阱。来自用户空间的陷阱的高级路径是`uservec`，然后是`usertrap`；返回时，先是`usertrapret`，然后是`userret`。

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

#text()[#h(2em)]如果是时钟中断（`which_dev == 2`），则增加时间片计数，当达到阈值（10个时间片）时调用`yield`。最后调用 `usertrapret()` 函数返回用户态，该函数负责恢复用户态执行环境。

== 内存管理

=== 地址空间

==== 内核链接地址

F7LY 在支持多架构内核时，并未采用"一体适配"的简化策略，而是根据架构自身特性量身定制内核布局，以保障启动路径的最小依赖性和最大清晰性。

#figure(
  image("fig/决赛内核地址空间.png", width: 90%),
  caption: [内核地址空间布局],
) <fig:address-layout>

#text()[#h(2em)]对于RISC-V，qemu的入口地址位于`0x8020_0000`。在此处，F7LY采用OPENSBI自动识别内核的ELF文件入口地址（LMA），低地址采用直接映射的方式映射所有MMIO设备以及内核代码。因为在进入到`entry.S`时仍然未开启分页，所以映射时低处地址采用直接映射便于访问。

与RISC-V使用 QEMU 默认跳转地址（`0x80200000`）不同，LoongArch 架构的启动顺序允许我们将内核加载地址与运行地址（VMA = LMA）分离，从而更灵活地构造分页映射策略。F7LY的LoongArch端应用loongarch的直接映射窗口，在链接脚本处使用`0x9000000080000000`加载内核镜像。


==== 地址空间布局

F7LY的地址空间设计参考了xv6的物理地址布局，采用分离的内核地址空间和用户地址空间。内核态页表管理整个内核地址空间，用户页表管理各个进程的用户地址空间。

对于内核地址空间，从KERNBASE开始映射内核映像；堆内存从地址`0x840000000`开始，剩余的空闲物理地址交由物理内存管理器管理，用于后续的动态分配。

对于用户地址空间，F7LY实现了现代化的进程内存管理架构，使用ProcessMemoryManager统一管理每个进程的内存布局：

#figure(
  image("fig/决赛用户地址空间.png", width: 85%),
  caption: [用户地址空间布局],
) <fig:user-address-layout>

- *程序段管理*：通过program_section_desc结构记录ELF加载的各个段（代码段、数据段、BSS段等），每个段包含起始地址、大小和调试名称，支持动态链接和静态链接程序。

- *堆内存管理*：使用heap_start和heap_end指针管理进程堆空间，堆空间紧接在程序段之后分配。通过grow_heap()和shrink_heap()接口支持动态扩展和收缩，与传统的brk系统调用兼容。

- *VMA管理*：实现虚拟内存区域（Virtual Memory Area）管理，支持mmap映射、文件映射、匿名映射、共享内存等高级内存管理功能。

- *引用计数管理*：ProcessMemoryManager实现了引用计数机制，支持进程间内存共享（如使用CLONE_VM标志的clone行为）。



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

#text()[#h(2em)]伙伴分配器(Buddy Allocator)通过分配和管理内存块来满足不同大小的内存请求，并进行高效的合并和分割操作。其优点在于分配和释放内存块的操作非常快速，且通过内存块大小的选择和合并操作，有效减少了外部碎片。但是对于页面内部的空间碎片，需要更细粒度的管理器进行分配，考虑到空闲物理空间与内核堆空间应用场景的不同，我们选择使用不同的细粒度分配器进行管理。

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

#text()[#h(2em)]`k_pmm`在初始化时会使用地址布局中的地址标志(`pa_start`)，原本Buddy的初始化放在这里，Buddy变成pmm的一个成员`pa_start`是buddy系统在物理内存中的起始地址，加上一个`Sizeof(BuddySystem)`后后面存的东西是tree，然后tree存完了之后才是buddy系统管理的那块内存。加上的BSSIZE是预留来放BuddySystem的大小和tree的大小，这之后才是buddy系统管理的那块内存，这时`pa_start`指向的就是buddy系统管理的那块内存的开始地址，再被初始化为buddy的基址。

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

#text()[#h(2em)]对于更细粒度的大小分配（函数`kmalloc`），F7LY采用了linux的`SlabAllocator`。slab allocator背后的思想是缓存经常使用的object并保持在初始状态供kernel使用。如果被基于object的allocator，内核将耗费很多时间在分配，初始化和释放相同的object。slab allocator的目的就是缓存一些被释放的object因此这些基础的structures在多次调用期间被预留起来。

#figure(
  image("fig/物理内存管理.png", width: 80%),
  caption: [物理内存管理],
) <fig:physical-memory-management>

#text()[#h(2em)]slab allocator由一组cache组成，这些cache由一个叫做cache chain的双向循环链表连接在一起。在slab allocator的上下文件，一个cache就是一个管理许多像`mm_struct`或者`fs_cache`这种特殊类型的object的管理者。这些cache通过cache struct的next字段连接在一起。

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

#text()[#h(2em)]Slab与Buddy二者分层次调用即可实现细粒度的物理内存分配，更小块的内存分配可以帮忙消除buddy allocator原本会造成的内部碎片问题。

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

#text()[#h(2em)]对于堆，设置管理类`HeapMemoryManager`并创建全局对象`k_hmm`进行管理。`k_hmm`在初始化时会从堆地址开始初始化Buddy System，并将此处划分为堆空间。

为提升堆分配的性能、减少碎片，我们在堆空间上引入了第三方高效内存分配器LibAllocator#footnote[https://github.com/blanham/liballoc]，主要用于进行高效内存管理。分配器设计为二层结构：上层是*粗粒度分配器（`L'Major`）*，负责大块内存获取；下层是*细粒度分配器（`L'Minor`）*，用于将大块切分成适用于常规 `new`/`malloc` 调用的空间，极大提升了小对象分配的效率。

F7LY 对 LibAllocator 进行了定制性适配，使其不再从物理内存中直接申请页帧，而是通过内核内部的 BuddyAllocator 分配大块内存作为 `L'Major` 的来源。这种结构层次分明、责任明确，既保持了 Buddy 分配器的可控性，也引入了 LibAllocator 在细粒度分配上的高效策略。

#figure(
  image("fig/buddy-allocator.png", width: 90%),
  caption: [Buddy Allocator 分配示意图],
) <fig:buddy-allocator>

#text()[#h(2em)]F7LY 重载了标准的 `new` / `delete` 运算符，使其默认在堆空间上分配内存并交由 `k_hmm` 管理：

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

#text()[#h(2em)]通过这种方式，内核中的对象创建和销毁行为得以统一，所有堆内分配操作均通过 `k_hmm` 路由，有效避免了裸指针操作和分配器混用的问题。

==== 地址空间管理

F7LY采用*静态预配置+管理器分派*的机制进行地址空间管理。核心由 `VirtualMemoryManager` 管理器统一负责虚拟地址的分配、映射与回收，底层则由 `PageTable` 类协助完成页表操作与维护。这种结构设计不仅提升了内存管理的确定性和效率，也使得地址空间的生命周期管理更为清晰。

针对 `mmap` 所涉及的文件映射与匿名映射，F7LY在每个进程控制块（PCB）中*预先分配了一块VMA表区域*，用于记录与管理该进程的虚拟内存区域（VMA），避免了运行时动态结构分配所带来的复杂性与不确定性。

*页表* #h(1em)PageTable 类用于抽象和管理多级页表结构。其核心成员变量包括：

- `base_addr`页表的物理基地址，指向页表的起始位置。
- `_is_global`标记该页表是否为全局页表（如内核页表）。

主要接口包括：

- `walk(uint64 va, bool alloc)`：软件递归遍历多级页表，查找或分配虚拟地址 va 对应的 PTE。可用于递归查找虚拟地址的映射关系，必要时分配中间页表。
- `walk_addr(uint64 va)`：返回虚拟地址 va 映射的物理地址指针。
- `freewalk()` / `freewalk_mapped()`：递归释放页表及其映射的物理页。
- `get_pte(index)` / `set_pte(index, pte)`：获取/设置指定索引的 PTE。

#text()[#h(2em)]在walk函数中，由于RISC-V使用SV39标准页表，而loongarch使用4级页表，二者不可统一，在此处F7LY分别定义了不同的实现，并在编译时根据宏进行区别。

*PTE* #h(1em)PTE类封装了单个页表项（Page Table Entry），通常包含如下信息：

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

#text()[#h(2em)]当用户态调用 `mmap` 系统调用时，内核会查找该进程 PCB 中的空闲 VMA 槽位，将映射信息记录其中，并返回分配好的虚拟地址。若为文件映射，则填入对应的 `vfile` 和 `offset` 字段；若为匿名映射，则标记 `flags` 中的 `MAP_ANONYMOUS`，并将 `vfile` 设为 `nullptr`。

==== 缺页异常处理

F7LY目前能够利用缺页异常处理来实现写时复制（Copy on write）、地址空间的懒分配（Lazy page allocation）以及用户的地址检查机制。

#figure(
  image("fig/缺页.png", width: 70%),
  caption: [缺页异常处理流程],
) <fig:page-fault>

#text()[#h(2em)]当用户程序因缺页异常进入内核时，两个架构的异常处理程序使用同样的处理逻辑，先检查缺页的地址是否处于物理空间，或处于vma记录的地址空间内，若是，则分配物理页面并建立映射。若不是，则抛出缺页错误。

== 进程管理

=== 进程控制块(PCB)

进程是操作系统中资源分配的基本单位。每个进程都有自己独立的地址空间和资源，如内存、文件描述符等。线程是操作系统中CPU调度的基本单位，线程共享所在的进程的地址空间和资源，但是有独立的执行上下文。

F7LY没有严格地区分进程和线程，统一使用Pcb结构体记录进程和线程，并在Pcb中设置globa_id、pid、tid、ppid、pgid等标准Linux进程标识符用于标记进程和线程状态，线程的共享内存状况也一并记录便于管理。

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

#text()[#h(2em)]进程状态的切换如下：

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

#text()[#h(2em)]`swtch`对线程没有直接的了解；它只是保存和恢复上下文（Contexts），`call_sched`调用`swtch`切换到`cpu->scheduler`，即每个CPU的调度程序上下文。调度程序上下文之前通过`scheduler`对`swtch`（`swtch(&p->_context, cpu` `->get_context());`）的调用进行了保存。当我们追踪`swtch`到返回时，它返回到`scheduler`而不是`sched`。

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

在F7LY内核中，进程使用独立的内存管理器来支持线程的创建和释放。虽然线程拥有独立的上下文，但它们位于同一进程中，且共享进程的地址空间。这种设计使得线程间可以高效共享内存，同时确保每个线程的上下文是独立的。为了支持线程的共享内存，我们使用了`ProcessMemoryManager`类的多种方法。具体而言，以下方法在F7LY中用于支持线程的共享内存操作：

```cpp
//为线程创建共享内存（增加引用计数）
ProcessMemoryManager *share_for_thread(); 
//为进程创建完全复制的内存管理器
ProcessMemoryManager *clone_for_fork(); 
```

#text()[#h(2em)]线程的创建与释放流程，尤其在`fork`和`clone`系统调用中，涉及以下步骤：

1. *分配与初始化子Pcb*：在调用`fork`和`clone`时，首先会为子进程分配并初始化一个新的进程控制块（Pcb）。    
2. *复制/共享打开文件*：在新创建的Pcb中，子进程会继承父进程的文件描述符，这些描述符指向相同的文件对象。 
3. *选择内存策略*：根据`clone`调用传入的标志位（如`CLONE_VM`），决定是否共享内存。
   - 不共享内存:如果不共享内存，子进程会创建一个完全独立的内存管理器。此时调用父进程的`clone_for_fork`方法，创建一个深度拷贝的内存管理器。在此过程中，系统为子进程分配一个全新的页表，并将`trampoline`、`sig_trampoline`以及程序段描述数组（`prog_sections`）与计数、堆元数据、VMA数据等内容进行复制。最终，复制后的进程内存管理器对象会与新进程关联。 
   - 共享内存:如果共享内存，子进程会调用父进程的`share_for_thread`方法，创建一个指向同一内存管理器的引用，并增加引用计数。这样，子进程和父进程将共享同一块内存区域，从而实现高效的内存使用。
4. *完成初始化与返回*：在完成进程的内存管理器设置后，继续进行其余的进程初始化工作，最终返回子Pcb对象。

==== 内存增长与回收

F7LY通过`ProcessMemoryManager`类提供了一系列方法来支持进程内存的动态增长与回收。这些方法涵盖了从堆内存的扩展与收缩，到虚拟内存区域（VMA）的管理等多个方面。通过这些接口，F7LY可以支持sbrk、brk、mmap系统调用为进程提供动态内存分配的支持。

此外，系统通过`/proc/stat`路径暴露进程的内存占用状态，方便外部工具或系统管理员查看进程的内存使用情况。

==== 内存释放

在F7LY中，进程内存的管理采用了统一的资源释放机制，以确保内存资源能够安全、准确地释放。我们通过`free_all_memory`方法来释放进程的所有内存资源，并且在内存资源的释放过程中，确保线程间的共享内存不会发生悬挂引用或重复释放的问题。

该方法的工作流程如下：
1. *检查共享标志*：首先，方法会检查进程内存管理器中的`shared_vm`标志，以确定当前进程是否与其他线程共享内存。
2. *处理共享内存*：在释放内存前，首先会减少共享内存的引用计数。只有当引用计数降为0时，内存资源才会被实际释放。这样确保了共享内存仅在最后一个引用被释放时才清理掉，从而避免了早期释放导致的悬挂引用问题。 
3. *释放内存资源*：如果当前进程不共享内存，或者引用计数已经降为0，方法会继续释放所有与进程相关的内存资源，包括页表、堆内存、VMA等。通过调用`pagetable.freewalk_mapped()`来递归释放页表及其映射的物理页，并清理其他内存区域。

=== 进程管理器

F7LY的进程管理使用类ProcessManager封装对进程的许多操作，并使用全局对象`k_pm`进行管理，既包含了对于当前进程的状态信息获取和修改接口，又作为系统调用和进程结构体这一个体之间的桥梁便于通过系统调用直接对进程进行操作。

下面将介绍这一类的公共接口与各字段的含义：

ProcessManager类的主要字段包括：

- `_pid_lock`：进程ID分配锁
- `_wait_lock`：等待操作锁  
- `_cur_pid`：当前分配的进程ID
- `_init_proc`：初始进程指针
- `_last_alloc_proc_gid_field`：上次分配的全局ID

```cpp
class ProcessManager
{
    // 进程管理的核心功能实现
    // 包括进程创建、销毁、调度等
};
```

#text()[#h(2em)]`ProcessManager`类的功能可以分为两个主要部分：

1. *进程状态管理*：
   - 分配与释放进程：`ProcessManager`负责进程的生命周期管理，包括进程的创建、销毁、以及资源的回收。每当一个新进程需要创建时，它会调用`ProcessManager`来分配进程ID（PID）并初始化进程相关的资源。当进程结束时，`ProcessManager`会负责清理并回收进程所占用的内存和其他资源。   
   - 进程属性设置:`ProcessManager`提供了一系列接口用于设置和获取进程的各种属性，如PID、PPID、PGID、UID、GID等。这些属性对于进程的管理和调度至关重要，确保每个进程都能正确地标识和管理其资源。
   - 进程状态转换：`ProcessManager`管理进程的状态转换，包括进程的运行、就绪、阻塞和终止等状态。它通过调度器与进程控制块（PCB）协同工作，确保进程能够在适当的时间点切换状态，并正确响应系统调用和中断。

2. *系统调用接口与进程生命周期*：
   - 系统调用处理：`ProcessManager`提供了多个系统调用接口，如`fork`、`execve`、`wait`、`exit`等。这些接口允许用户态程序通过系统调用与内核交互，执行进程创建、程序替换、进程等待和终止等操作。
   - 进程调度与切换：`ProcessManager`与调度器协同工作，管理进程的调度和上下文切换。它确保在多任务环境下，CPU资源能够公平地分配给各个进程，并根据优先级和状态进行调度。
   - 信号处理与进程间通信：`ProcessManager`还负责处理进程间的信号传递和通信机制，确保进程能够响应外部事件和内部状态变化。

#text()[#h(2em)]执行用户态程序的流程是：首先使用`fork`创建一个子进程，然后通过`execve`加载ELF文件中的用户程序，将原进程的内存空间和执行上下文替换为新程序的资源，并从ELF文件定义的入口地址（entry）开始执行。

当用户态程序需要申请内核资源或执行特权操作时，会通过系统调用进入内核。这一过程由硬件触发用户态到内核态的陷入（`usertrap`），在陷入点内核会根据异常码进行判断，并通过`syscall_handler`包装逻辑进入具体的系统调用处理流程。

#figure(
  image("fig/进程架构.png", width: 80%),
  caption: [双管理器进程架构],
) <fig:process-architecture>

== 文件系统架构

=== 虚拟文件系统

虚拟文件系统（Virtual File System, VFS）是操作系统内核中的关键子系统，负责统一管理磁盘文件、I/O设备等字符流的交互，为上层提供一致、透明的文件访问接口。VFS通过标准化的系统调用（如`open()`、`read()`、`write()`）屏蔽了底层文件系统和存储介质的差异，使用户程序能够以统一的方式访问不同类型的文件系统。

在F7LY的设计过程中，初赛时参考的往年队伍XN6的文件系统存在不完善之处，这使得后续的开发工作遇到了许多困难。尤其是C++的虚类继承机制在文件操作时导致了多次解码，影响了性能。因此，在决赛阶段，F7LY决定移植成熟的*lwext4库*，并基于此实现了一个面向过程调用的VFS接口。为了保持高效的性能，我们在虚拟文件管理时使用了一层简单的虚拟文件封装，通过这一层封装来选择正确的文件类型与操作。

==== 虚拟文件架构

1. *底层元数据与文件结构*：
   在VFS的底层，我们使用了`lwext4`库中的`lwext4_file_struct`结构体，它提供了对文件元数据的管理，包括关键字段：  
   - `fsize`：文件大小。
   - `fpos`：当前文件位置读写偏移指针。
   - `flags`：文件标志位，指示文件的打开模式（如只读、可写等）。
   - `inode`：指向文件的索引节点（Inode），包含文件的元数据。

2. *虚拟文件类file*：
   为了管理打开的文件，F7LY设计了一个`file`类，该类是一个虚类，用于表示进程中打开文件的状态属性。`file`类包含了以下关键字段：  
   - `lwext4_file_struct`：指向底层文件结构的指针，封装了文件的元数据。
   - `st`：一个`Kstat`结构体，包含文件的状态信息，如文件大小、访问时间等。
   - `attrs`：一个`FileAttrs`结构体，包含文件的属性信息，如权限、类型等。
   - `file_ptr`：文件的读写指针，为支持稀疏文件的高效读写提供了便利。
   
   这个类不仅包含了文件的基本信息，还通过虚函数为不同的文件类型提供了不同的操作接口。为避免直接操作底层元数据，这一层的封装同步存储了文件大小等数据，旨在避免造成lwext4库的破坏（该库为c语言，没有封装性），并在上层能更直观的获取到数据。

3. *虚拟文件类的继承结构*：
   根据Linux"一切皆文件"的设计理念，F7LY的`file`类被派生出了多个具体的文件类型。这些派生类包括：
   - `normal_file`：表示普通文件，提供了对普通文件的读写操作。
   - `device_file`：表示设备文件，提供了对设备的特殊操作。
   - `pipe_file`：表示管道文件，提供了对管道的读写操作。
   - `socket_file`：表示套接字文件，提供了对网络套接字的操作。
   - `dir_file`：表示目录文件，提供了对目录的遍历和操作。
   - `virtual_file`：表示虚拟文件，提供对系统状态文件的操作。
   
   每种文件类型根据其特性，提供不同的读写操作。通过虚函数重载，F7LY能够为每个文件类型实现特定的读写操作，确保文件操作的多样性与高效性。

#figure(
  image("fig/虚拟文件.png", width: 70%),
  caption: [虚拟文件类继承结构],
) <fig:virtual-file-hierarchy>

==== 虚拟文件系统架构

F7LY内核目前仅支持ext4文件系统，但由于系统状态文件（如`proc`、`sys`等）并不存储在ext4文件系统中，因此我们设计了一个简单的VFS架构来支持多种文件系统的挂载与管理。VFS通过标准化的接口，屏蔽了底层文件系统的差异，使得用户程序能够以统一的方式访问不同类型的文件系统。

在这个vfs中，底层的ext4文件系统通过`lwext4`库进行管理，元数据结构如超级块、inode等均由该库提供支持，文件的读写操作通过`normal_file`类进行封装和管理。而与磁盘的交互则通过`BlockDevice`类进行处理，使用`buf`结构体统一块大小的访问，确保了对物理存储设备的统一访问接口。

而系统状态文件则通过`virtual_fs`类进行管理，在初始化时会使用虚拟文件的`VirtualContentProvider`来创建和管理这些文件，标记这些文件为动态或静态以便后续与内核状态同步。这样，F7LY的VFS能够同时支持持久化存储的文件系统和非持久化的系统状态文件，提供了灵活且高效的文件访问机制。

#figure(
  image("fig/文件系统构筑.png", width: 75%),
  caption: [虚拟文件系统架构],
) <fig:vfs-architecture>

#text()[#h(2em)]目前状态文件支持的路径包括：
- `/proc/`：包含进程信息、系统状态等动态内容。
- `/sys/`：包含系统硬件信息、内核参数等静态内容。
- `/dev/`：包含设备文件，提供对硬件设备的访问接口。

==== VFS与性能的优化

为了确保高效的性能，F7LY采取了如下优化策略：
- open等全局的文件操作通过使用面向过程调用的VFS接口，避免了C++虚类继承机制所带来的性能开销，尤其是在频繁进行文件操作时。    
- 虚拟文件管理仅在必要时使用虚拟文件封装，从而确保在文件操作时能够快速定位并执行正确的文件类型操作。 
- 文件读写这类基础操作通过虚函数重载的方式来处理不同类型的文件，实现了代码的灵活性和可扩展性，同时确保了操作的高效性。
#figure(
  image("fig/文件操作.png", width: 75%),
  caption: [两层封装的文件操作],
) <fig:file-operation>
#text()[#h(2em)]通过两次封装，F7LY的VFS能够在保证灵活性的同时，提供高效的文件操作性能。第一层封装是对lwext4库的封装，第二层则是对虚拟文件类的封装，这样既能利用现有成熟库的稳定性，又能在上层提供统一的接口供用户程序调用。

=== VFS核心元数据结构剖析

==== Buffer

Buffer用于管理磁盘数据的内存缓冲区。它提供了磁盘扇区数据在内存中的抽象表示。F7LY的缓冲区容器（BufferBlock），组织和管理一组相关的磁盘缓冲区，用于组成了链表节点，串接而成一块缓冲数据链。

#figure(
  image("fig/os-buffer-pool.png", width: 75%),
  caption: [Buffer缓冲区管理],
) <fig:buffer-pool>

==== SuperBlock

超级块作为文件系统的核心元数据结构，承担着存储文件系统全局配置信息的重要职责。在物理存储层面，该结构通常映射到磁盘特定位置的元数据存储区域。从面向对象的角度来看，每个超级块实例都代表着一个具体的文件系统实例化对象。

针对基于持久化存储的文件系统，其生命周期管理包含以下关键流程：

- *挂载阶段*：内核需要从磁盘元数据区域读取原始超级块信息，并在内存中构建对应的运行时数据结构；
- *卸载阶段*：系统需要执行相反的操作，包括释放内存中的超级块对象，并将修改后的元数据同步回持久化存储设备。

而对于非持久化文件系统（如内存文件系统sysfs、procfs等），其超级块管理则简化为仅需在内存空间维护独立的元数据结构，无需考虑与物理存储设备的同步问题。这种差异化的实现机制充分体现了VFS设计对不同存储介质的良好适应性。

F7LY的VFS超级块由superblock定义：

```cpp
struct superblock {
    uint8 s_dev; //块设备标识符
    uint32 s_blocksize; //数据块大小，字节单位

    uint32 s_magic; //文件系统的魔数
    uint32 s_maxbytes; //最大文件大小
    inode_ptr root; //指根目录

    super_operations_ptr s_op;

    SpinLock dirty_lock;
    list_head s_dirty_inodes; //脏inode表
};
```

对于具体的文件系统，只需要移植自己的超级块结构体，并完善同样的函数实现。如ext4的超级块：

```cpp
struct ext4_sblock {
    uint32_t inodes_count; /* I-nodes count */
    uint32_t blocks_count_lo; /* Blocks count */
    uint32_t reserved_blocks_count_lo; /* Reserved blocks count */
    uint32_t free_blocks_count_lo; /* Free blocks count */
    uint32_t free_inodes_count; /* Free inodes count */
    uint32_t first_data_block; /* First Data Block */
    uint32_t log_block_size; /* Block size */
    uint32_t log_cluster_size; /* Obsoleted fragment size */
    uint32_t blocks_per_group; /* Number of blocks per group */
    uint32_t frags_per_group; /* Obsoleted fragments per group */
    uint32_t inodes_per_group; /* Number of inodes per group */
    uint32_t mount_time; /* Mount time */
    uint32_t write_time; /* Write time */
    uint16_t mount_count; /* Mount count */
    uint16_t max_mount_count; /* Maximal mount count */
    uint16_t magic; /* Magic signature */
    uint16_t state; /* File system state */
    uint16_t errors; /* Behavior when detecting errors */
    uint16_t minor_rev_level; /* Minor revision level */
    uint32_t last_check_time; /* Time of last check */
    // ... 省略其他字段
}
```

==== Inode

在F7LY中，*Inode（索引节点）是文件系统的核心元数据结构*，我们通过独立于文件名的Inode编号唯一标识每个文件或目录，并在其中存储文件类型、权限、所有者、大小、时间戳、数据块位置等关键信息。F7LY的底层文件系统采用多级索引机制：直接指针用于快速访问小文件数据，间接指针支持大文件的存储扩展，从而兼顾性能与可扩展性。

在虚拟文件系统层（VFS），F7LY定义了一个通用的Inode结构体，包含文件类型、权限、锁机制、操作函数指针等字段，并通过函数指针实现对不同文件系统的操作抽象。这样，VFS可以统一管理各种文件系统的Inode操作，如读写、锁定、更新等。

```cpp
struct inode {
    uint8 i_dev;
    uint16 i_mode; //类型 & 访问权限
    //...省略其他字段 
    SpinLock lock; //测试完成后再换成信号量
    struct inode_operations *i_op; //inode操作函数
    struct superblock *i_sb;
    struct vfs_ext4_inode_info i_info; //EXT4 inode结构
};

struct inode_operations {
    void (*unlockput)(struct inode *self);
    void (*unlock)(struct inode *self);
    void (*put)(struct inode *self);
    void (*lock)(struct inode *self);
    void (*update)(struct inode *self);
    ssize_t (*read)(struct inode *self, int user_dst, uint64 dst, uint off, uint n);
    int (*write)(struct inode *self, int user_src, uint64 src, uint off, uint n);
    int (*isdir)(struct inode *self); // 是否是directory
    struct inode *(*dup)(struct inode *self);
    //For directory
    struct inode *(*dirlookup)(struct inode *self, const char *name, uint *poff);
    int (*deletei)(struct inode *self, struct inode *ip);            
    int (*dir_empty)(struct inode *self);
    struct inode *(*create)(struct inode *self, const char *name, uchar type, short major, short minor);
    void (*stat)(struct inode *self, struct stat *st);
};
```

=== 系统文件访问

F7LY内核实现了完整的虚拟文件系统，为应用程序提供了类Linux的系统文件接口。虚拟文件系统通过树形结构组织，支持多种文件类型，包括普通文件、符号链接和设备文件。系统在初始化时会创建以下虚拟文件，并标记动态和静态：

*\/proc文件系统：*
- `/proc/self/exe` - 当前进程可执行文件的符号链接
- `/proc/meminfo` - 系统内存使用信息
- `/proc/cpuinfo` - CPU硬件信息
- `/proc/version` - 内核版本信息
- `/proc/mounts` - 文件系统挂载信息
- `/proc/self/mounts` - 当前进程的挂载信息
- `/proc/self/cmdline` - 当前进程命令行参数
- `/proc/self/stat` - 当前进程状态统计
- `/proc/self/maps` - 当前进程内存映射信息
- `/proc/self/pagemap` - 当前进程页面映射
- `/proc/self/status` - 当前进程详细状态
- `/proc/<pid>/stat` - 指定进程的状态统计
- `/proc/stat` - 系统统计信息
- `/proc/uptime` - 系统运行时间
- `/proc/interrupts` - 中断统计信息

*\/proc/sys内核参数：*
- `/proc/sys/kernel/pid_max` - 最大进程ID
- `/proc/sys/kernel/shmmax` - 共享内存段最大大小
- `/proc/sys/kernel/shmmni` - 共享内存段最大数量
- `/proc/sys/kernel/shmall` - 共享内存总大小限制
- `/proc/sys/kernel/tainted` - 内核污染状态
- `/proc/sys/fs/pipe-user-pages-soft` - 管道用户页面软限制

*\/etc配置文件：*
- `/etc/passwd` - 用户账户信息
- `/etc/ld.so.preload` - 动态链接器预加载库配置
- `/etc/ld.so.cache` - 动态链接器缓存文件

*\/dev设备文件：*
- `/dev/null` - 空设备，丢弃所有写入数据
- `/dev/zero` - 零设备，读取时返回零字节
- `/dev/loop-control` - Loop设备控制接口
- `/dev/loop0` - `/dev/loop7` - Loop块设备，支持文件系统镜像挂载
- `/dev/block/8:0` - 块设备文件

#text()[#h(2em)]特别地，F7LY的Loop设备支持使得系统能够将文件作为块设备进行挂载，这为文件系统镜像的使用和测试提供了重要支持。通过Loop设备，用户可以挂载ISO镜像、磁盘镜像等文件，极大地扩展了文件系统的灵活性。

这些虚拟文件通过专门的Provider类实现，每个Provider负责生成对应文件的内容，确保了系统信息的实时性和准确性。虚拟文件系统的实现使得F7LY能够很好地兼容标准的Linux应用程序和系统工具。
=== 额外的文件功能
在基础文件读写之外，F7LY 还实现了部分 Linux 系统调用所涉及的扩展文件功能，涵盖文件控制接口（`fcntl`）以及扩展属性（xattr 和 ioctl），以增强文件系统的兼容性和灵活性。
==== 文件控制（fcntl）
`fcntl` 系统调用在 UNIX/Linux 系统中用于对文件描述符进行多样化的管理。F7LY 目前已经支持若干常用的 `op` 操作，其入口函数为  
`SyscallHandler::sys_fcntl()`，参数解析使用 `_arg_fd`、`_arg_int` 和 `_arg_addr`。  
实现流程为：根据传入的 `fd` 获取对应的 `fs::file*`，再依据不同的 `op` 进行分支处理。主要功能如下：
#figure(
  image("fig/fcntl.png", width: 75%),
  caption: [文件控制调用],
) <fig:fcntl>
1. 文件描述符复制（`F_DUPFD` /` F_DUPFD_CLOEXEC`）   
    - 调用 `proc::k_pm.alloc_fd(p, f, i)` 分配新的文件描述符。        
    - 在进程文件表中记录：`p->_ofile->_ofile_ptr[retfd] = f`，同时增加 `f->refcnt`。        
    - `F_DUPFD_CLOEXEC` 额外设置 `p->_ofile->_fl_cloexec[retfd] = true`。        
2. 文件描述符标志（`F_GETFD` / `F_SETFD`）    
    - `FD_CLOEXEC` 标志由 `p->_ofile->_fl_cloexec[fd]` 维护。        
    - `F_GETFD` 返回当前标志值；`F_SETFD` 根据参数设置或清除该标志。        
3. 文件状态标志（`F_GETFL` /`F_SETFL`）    
    - 文件的状态标志存储在 `f->lwext4_file_struct.flags`。        
    - 对管道文件，`O_NONBLOCK` 的设置通过 `fs::pipe_file` 接口进行同步。        
    - `F_SETFL` 仅允许修改以下标志：\
        `O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK`，  
        而访问模式位（`O_RDONLY`/`O_WRONLY`/`O_RDWR`）保持不变。        
4. 记录锁（`F_SETLK` / `F_SETLKW` / `F_GETLK`）
    - F7LY 实现了简化的 advisory record locking：        
        - 每个文件维护一个 `struct flock`（`f->_lock`），用于记录锁状态。            
        - `F_SETLK`：设置锁，当前仅支持单锁存储，权限和竞争处理有限。            
        - `F_SETLKW`：在冲突时阻塞等待写操作。            
        - `F_GETLK`：检查冲突并返回冲突信息或 `F_UNLCK`。              
5. 管道相关（`F_SETPIPE_SZ` / `F_GETPIPE_SZ`）   
    - 针对 `f->_attrs.filetype == FT_PIPE` 的文件，调用 `set_pipe_size()` / `get_pipe_size()` 管理管道容量。        
    - 实现在 `kernel/proc/pipe.cc` 与 `kernel/fs/vfs/file/pipe_file.hh`。        
6. 文件封印（`F_ADD_SEALS`, `F_GET_SEALS`）  
    - 用于 `memfd_create` 的支持，仅当路径名前缀为 `memfd:` 时启用。        
    - 实现了四种封印：        
        - `F_SEAL_SHRINK`            
        - `F_SEAL_WRITE`            
        - `F_SEAL_GROW`            
        - `F_SEAL_SEAL`（启用后禁止添加新的封印）            
    - 封印状态存储在 `f->_seals` 与 `f->_sealing_allowed` 字段中。        
==== 文件扩展属性
F7LY 在移植 `lwext4` 库时，保留了其完整的扩展属性支持，并在 VFS 层进行了封装，实现了如下功能：
==== *xattr 支持*    
    - `lwext4` 子系统实现了完整的 xattr 接口：        
        - `sys_setxattr`, `sys_lsetxattr`, `sys_fsetxattr`            
        - `sys_getxattr`, `sys_lgetxattr`, `sys_fgetxattr`            
    - 提供 `set/get/list/remove` 等操作，能够满足常见的扩展属性管理需求。        
==== *inode flags 管理*   
    - 通过 `ioctl` 系统调用支持 `FS_IOC_GETFLAGS (0x6601)` 与 `FS_IOC_SETFLAGS (0x6602)`，可对 ext4 inode flags 进行查询和设置。

== 进程间通信

=== 信号机制

信号是操作系统向进程传递异步事件通知的重要手段，广泛用于异常处理、进程控制和进程间通信。F7LY借鉴Linux的设计，支持符合POSIX标准的信号机制，并在PCB中内置信号处理所需的关键数据结构。

==== 核心数据结构

F7LY定义了以下结构体以支持灵活、兼容的信号处理：

```cpp
namespace signal
{
    struct signal_frame
    {
        sigset_t mask;
        TrapFrame tf;
        signal_frame *next;
    };
    typedef struct sigaction
    {
        __sighandler_t sa_handler; // 信号处理函数
        uint64 sa_flags;         // 行为标志
        uint64 sa_restorer;      // 恢复函数
        sigset_t sa_mask;        // 处理期间阻塞的信号
    } sigaction;
}
```

#text()[#h(2em)]这些结构体与POSIX标准中的`siginfo_t`语义接近，增强了对标准接口的兼容性；

- `signal_frame`用于保存信号处理过程中的进程上下文；
- `sigaction`定义每个信号对应的处理行为。

==== 信号处理流程

在任务从内核态返回用户态前，需要处理挂起的信号。F7LY在中断处理器`usertrap`中加入`handle_signal()`，在时钟中断或其他中断后及时执行挂起信号：

```cpp
if (which_dev == 2)
{
  timeslice++; 
  if (timeslice >= 10)
  {
    timeslice = 0;
    printf("yield in usertrap
");
    proc::k_scheduler.yield();
  }
}
handle_signal();
usertrapret();
```

`handle_signal()`的核心逻辑是：

1. 获取当前进程PCB；
2. 遍历进程挂起的信号位掩码，检查每个信号的有效性；
3. 根据`sigaction`判断信号是否需要自定义处理：
   - 若`sa_handler == nullptr`或者`sa_handler == SIG_DFL`，使用默认处理（例如SIGKILL设置`p->_killed=true`）；
   - 若`sa_handler == SIG_IGN`，忽略该信号；
   - 否则调用用户自定义的信号处理函数；
4. 处理完当前信号后不会丢失其他信号，通过位掩码保留剩余信号，保证处理安全性。

==== 默认信号处理函数

当进程没有为特定信号设置自定义处理函数时，F7LY内核会调用默认的信号处理函数`default_handle()`。该函数根据POSIX标准的信号语义，为不同类型的信号提供相应的默认行为。

F7LY通过`SignalAction`结构体定义信号的默认行为：

```cpp
struct SignalAction {
    bool terminate;    // 是否终止进程
    bool coredump;     // 是否生成core dump
};
```

==== 用户自定义信号处理

F7LY实现了完整的用户自定义信号处理机制，支持完整的上下文保存与恢复：

- 当内核准备进入用户态执行信号处理函数时，会将当前上下文保存到`sig_trapframe`中；
- 信号处理完成后，程序需回到原执行状态。此过程使用`sigreturn`系统调用恢复上下文。

为了实现从信号处理返回，F7LY设计了类似`trampoline`的机制：

- 信号处理时将EPC（返回地址）设置到汇编实现的`sig_trampoline`函数；
- `sig_trampoline`唯一作用是触发`ecall`，调用`SYS_rt_sigreturn`系统调用，执行上下文恢复。

#text()[#h(2em)]这一设计使得F7LY在支持自定义信号处理时能保证上下文正确性，安全地完成用户态与内核态的切换。

对于flag中包含了`SA_SIGINFO`的信号，F7LY支持传递`LinuxSigInfo`结构体作为参数，提供更多信号上下文信息:
- 在进入信号处理函数时，内核会将装填`LinuxSigInfo`结构体, 并将其存放到栈上传递给用户态处理函数。
- 在用户态调用`sigreturn`时，内核会解析`LinuxSigInfo`结构体，并将其恢复到用户态栈中。

```cpp
// SA_SIGINFO标志的处理
if (act->sa_flags & SA_SIGINFO) {
    // 构造LinuxSigInfo结构
    LinuxSigInfo siginfo = {
        .si_signo = (uint32)signum,
        .si_errno = 0,
        .si_code = 0
    };
    
    // 将siginfo和ucontext写入用户栈
    uint64 linuxinfo_sp = usercontext_sp - sizeof(LinuxSigInfo);
    mem::k_vmm.copy_out(\*p->get_pagetable(), linuxinfo_sp, 
                       &siginfo, sizeof(LinuxSigInfo));
    
    // 设置三参数信号处理函数的参数
    p->_trapframe->a0 = signum;         // 信号编号
    p->_trapframe->a1 = linuxinfo_sp;   // siginfo_t*
    p->_trapframe->a2 = usercontext_sp; // ucontext_t*
}
```

#text()[#h(2em)]同时，F7LY还在栈顶设置了信号哨兵`guard`，用于检测栈溢出或非法访问。信号处理函数在执行前会在栈上压入哨兵值，返回值检查哨兵值是否一致，确保栈空间安全。

#figure(
  image("fig/信号处理.png", width: 70%),
  caption: [信号处理流程],
) <fig:signal-handling>

==== 信号与系统调用的集成

F7LY的信号机制支持包括`SIGCHLD`在内的常用信号。例如，当子进程状态变为Zombie时，内核通过发送`SIGCHLD`信号告知父进程状态变化，同时提供子进程的PID、状态码、用户态和内核态运行时间等信息，为父进程调用`wait4()`等系统调用获取子进程状态提供必要数据。

在系统调用执行过程中，F7LY内核会根据不同的事件触发相应的信号发送：

- *进程异常终止*：当进程因段错误、非法指令等异常终止时，内核向父进程发送`SIGCHLD`信号，并设置相应的退出状态码。
- *内存访问违规*：当进程访问无效内存地址或违反内存保护时，内核向该进程发送`SIGSEGV`信号，触发段错误处理。
- *资源限制违规*：当进程超出系统资源限制（如CPU时间、文件大小）时，内核发送相应的信号（`SIGXCPU`、`SIGXFSZ`）通知进程。

#text()[#h(2em)]这种信号与系统调用的紧密集成确保了F7LY能够及时响应各种系统事件，为用户态程序提供可靠的异步通知机制。

=== Futex

Futex（Fast Userspace Mutex）是Linux内核提供的一种高效用户态同步原语，可用于实现互斥锁、条件变量、信号量等多种同步机制。F7LY借鉴Linux的设计，构建了可扩展的Futex实现，并支持相关系统调用。

==== Robust list

F7LY使用`robust_list`结构体作为构成单向链表的节点嵌入在用户空间的锁结构中。内核只需要知道前向链接，用户空间可以使用双向链表实现O(1)的添加和删除。

- *`list`*：robust locks链表的头节点，如果为空则指向自己。
- *`futex_offset`*：用户空间设置的相对偏移量，告诉内核futex字段在数据结构中的位置。
- *`list_op_pending`*：防止线程死亡竞争的字段。
  - 用户空间首先将此字段设置为即将获取的锁的地址。
  - 然后执行锁获取操作。
  - 再将自己添加到列表中。
  - 最后清除此字段。

```cpp
struct robust_list {
    struct robust_list *next;
};
struct robust_list_head {
    robust_list list;           // 链表头
    long futex_offset;          // futex 字段的相对偏移
    robust_list *list_op_pending; // 待处理的锁操作
};
```

#text()[#h(2em)]Robust Futex的意义是当线程异常终止时，内核可以自动清理该线程持有的锁，防止死锁。

==== Futex Wait 与 Wakeup 机制

F7LY实现的futex系统调用遵循Linux标准，提供了`futex_wait`和`futex_wakeup`两个核心操作。

```cpp
int futex_wait(uint64 uaddr, int val, tmm::timespec *timeout);
int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2);
```

*工作机制*

`futex_wait`首先原子性地检查用户地址`uaddr`处的值是否等于期望值`val`，若不匹配则立即返回`EAGAIN`；若匹配，则将当前进程设置为`SLEEPING`状态并记录等待通道，支持超时和信号中断处理。

`futex_wakeup`通过进程管理器查找等待在目标地址的进程，将其从`SLEEPING`状态恢复到`RUNNABLE`状态，返回实际唤醒的进程数量。

#figure(
  image("fig/futex.png", width: 70%),
  caption: [futex工作机制],
) <fig:signal-handling>

#text()[#h(2em)]这种设计实现了高效的用户态快速路径：锁可用时直接获取，需要阻塞时才进入内核，显著减少系统调用开销。Futex是现代多线程程序同步的基础设施，可用于实现互斥锁、条件变量等高级同步原语。

=== 共享内存机制

共享内存是一种高效的进程间通信（IPC）机制，允许多个进程直接访问同一块物理内存区域，从而实现数据的快速交换。F7LY借鉴Linux设计的同时改用面向对象的管理方式，支持符合POSIX标准的共享内存机制，并在内核中实现了相关系统调用。

F7LY的内核空间通过类`SharedMemoryManager`，并设立全局对象`k_smm`来管理共享内存区域。该类负责创建、销毁和跟踪共享内存段的生命周期，确保多个进程能够安全地访问和操作共享内存。

该类的定义如下：
```cpp
class ShmManager
{
private:
    unordered_map segments; // 共享内存段映射
    int next_shmid; 
    uint64 shm_base;
    uint64 shm_size;
    vector free_blocks;
public:
    int create_seg(key_t key, size_t size, int shmflg);
    int delete_seg(int shmid);
    void_ptr attach_seg(int shmid, void_ptr shmaddr, int shmflg);
    int detach_seg(void_ptr addr);
    bool is_shared_memory_address(void_ptr addr);
    bool find_shared_memory_segment(/* ... */);
    bool add_reference_for_fork(void_ptr addr);
    int shmctl(int shmid, int cmd, shmid_ds_ptr buf, uint64 buf_addr);
    //...省略其他函数
};
```

==== 共享内存段结构体

该类封装了一个共享内存段的列表，每个共享内存段由`shm_segment`类表示，包含以下关键字段：
```cpp
   struct shm_segment
    {
        int shmid;         // 共享内存段ID
        key_t key;         // 共享内存段键值
        size_t size;       // 用户请求的原始大小
        size_t real_size;  // 实际分配的页对齐大小
        union
        {
            u16 shmflg;
            struct
            {
                u16 o_exec : 1;  // 其他执行权限
                u16 o_write : 1; // 其他写权限
                u16 o_read : 1;  // 其他读权限
                u16 g_exec : 1;  // 组执行权限
                u16 g_write : 1; // 组写权限
                u16 g_read : 1;  // 组读权限
                u16 u_exec : 1;  // 用户执行权限
                u16 u_write : 1; // 用户写权限
                u16 u_read : 1;  // 用户读权限
                u16 _rsv : 7;
            }__attribute__((__packed__));
        }__attribute__((__packed__));
        eastl::vector<pair<tid,void*>> attached_addrs; 
        uint64 phy_addrs;  // 物理地址
        int nattch;        // 当前附加的进程数量
        ....//省略其他字段
    };
```

该结构体与linux的shm_segment结构体兼容，包含了共享内存段的ID、键值、大小、权限标志等信息。与原本的linux结构体不同，它维护了一个附加地址列表，用于跟踪所有附加到该共享内存段的虚拟地址并存储附加地址的唯一tid，该方法可以支持多个进程附加到同一个共享内存段，并使用键值区别不同页表上的附加值。

==== 共享内存的实现

在设计`SharedMemoryManager`的类方法时，F7LY直接参考了posix标准的共享内存API，并实现了以下关键方法：
- `create_seg`：该方法对标shmget系统调用，创建一个新的共享内存段，并返回其ID。该方法接受三个参数：键值、大小和标志位。它会检查是否已经存在同样的键值的共享内存段，如果存在则返回其ID，否则创建新的段并返回新段的ID。
- `delete_seg`：该方法对标shmctl系统调用中的删除逻辑，但给出更灵活的api，删除指定的共享内存段。它会检查该段是否存在，并且只有附加地址列表为空，且设置删除标志（`SHM_DEST`）时才会被调用删除，否则减少附加计数并按标准更新段信息。删除后会释放物理内存并从段列表中移除。
- `attach_seg`：该方法对标shmat系统调用，允许进程附加到指定的共享内存段。它会检查是否已经存在附加地址，如果不存在则分配新的虚拟地址，并将其添加到附加地址列表中。若指定了附加地址，则直接使用该地址。附加的同时记录此次附加的进程ID，并增加附加计数。若附加成功，则返回附加地址。
- `detach_seg`：该方法对标shmdt系统调用，允许进程从共享内存段中分离。它会检查指定的地址是否属于共享内存段，并从附加地址列表中移除该地址，同时减少附加计数。如果附加计数为0且设置了删除标志，则释放物理内存并从段列表中移除该段。
- `is_shared_memory_address`：检查指定地址是否属于共享内存段，返回布尔值。主要用于查找和验证附加地址是否有效，并进入共享内存处理逻辑。
- `shmctl`：该方法对标shmctl系统调用，提供对共享内存段的控制操作，如获取段信息、设置权限等。它接受三个参数：共享内存段ID、命令和缓冲区地址。根据命令执行不同的操作，如获取段信息、设置权限等。

这些方法实现了共享内存段的创建、删除、附加和分离等基本操作，符合POSIX标准的共享内存API规范。

==== 使用ShmManager实现mmap的共享内存

在F7LY的实现中，PCB没有直接存储共享内存段的引用计数，而是通过`SharedMemoryManager`类来管理所有共享内存段的生命周期。每个进程在附加共享内存段时，都会调用`attach_seg`方法，该方法会检查是否已经存在附加地址，并更新附加地址列表。

F7LY的`mmap`系统调用支持共享内存段的映射。通过`SharedMemoryManager`类的`attach_seg`方法，进程可以将共享内存段映射到自己的虚拟地址空间中。

当进程调用`mmap`时，若包含flag`MAP_SHARED`，则会触发共享内存段的附加操作。在一般的mmap操作时，会在用户堆顶选取虚拟地址，并使用`PhysicalMemoryManager`分配物理页。若是共享内存段，则会调用`create_seg`方法并传入特定的key值创建一个不会重复的共享内存段。并调用`attach_seg`方法将共享内存段附加到进程的虚拟地址空间中。

```cpp
  bool is_shared_memory_address(void *addr);
  int find_shared_memory_segment(void *addr, 
  void **start_addr, size_t *size = nullptr);
  bool add_reference_for_fork(void *addr);
  bool duplicate_attachments_for_fork(uint parent_tid, uint child_tid);
```

#text()[#h(2em)]共享的vma数据会在fork时跟随`ProcessMemoryManager`一起管理，进程复制时使用相关接口检查地址是否属于共享内存段，并获取共享内存段信息。若是共享内存段，则增加引用计数，并在子进程中复制附加地址。

此处区分进程与线程的共享内存段管理，根据传入的`CLONE_VM`标志不同进行不同处理，确保每个进程在fork时能够正确处理共享内存段的引用计数和附加地址。

==== 通过/dev/shm访问共享内存

F7LY的共享内存段可以通过`/dev/shm`目录访问。在初始化目录时会通过`VirtualContentProvider`创建标志为动态的虚拟目录，该目录下的文件对应于共享内存段，每个文件名为共享内存段的键值（key）。

用户可以通过标准的文件操作接口（如`open`、`read`、`write`等）来访问这些共享内存段。这些访问的请求将会转发到`ShmManager`类的相应方法中进行处理。

例如，用户可以通过以下方式创建和访问共享内存段：
```cpp
int fd = open("/dev/shm/my_shared_memory",
              O_RDWR | O_CREAT, 0666);
if (fd < 0) {
    perror("open");
    return -1;
}   
```

#text()[#h(2em)]特别的，LTP测试中初始化阶段会使用下面的方法设置好IPC的共享内存段，并以此存储测试的结果。
```cpp
static void setup_ipc(void)
{
    size_t size = getpagesize();
    if (access("/dev/shm", F_OK) == 0) {
        snprintf(shm_path, sizeof(shm_path), "/dev/shm/ltp_%s_%d",
            tid, getpid());
    } else {
        //...其余进程通信逻辑
    }
}
```

#text()[#h(2em)]这样的方法可以确保每个进程在访问共享内存时都能正确地创建和使用对应的共享内存段。

#figure(
  image("fig/共享内存空间管理.png", width: 75%),
  caption: [共享内存管理接口],
) <fig:shared-memory>

=== *memfd机制*
在现代 Linux 系统中，`memfd_create` 提供了一种高效的共享内存机制：  
发送方进程首先调用 `memfd_create` 创建一个内存文件，然后通过 `mmap` 将其映射到进程地址空间，从而获得一块可读写的共享内存区域。此时，进程可以向这块内存写入数据，并通过 Unix Domain Socket 将文件描述符发送给接收方进程，后者接收并映射同一个内存文件，从而实现高效的跨进程共享。
==== F7LY的memfd实现
在F7LY 内核中，我们完整实现了这一机制，并针对比赛环境进行了适配与优化：
- `memfd` 的创建与标识    
    - 在 `sys_memfd_create` 中，F7LY 使用 VFS 的 `O_TMPFILE` 机制在 `/tmp` 下创建匿名文件，并将其 `_path_name` 标记为 `memfd:<name>`，以区分普通临时文件。        
    - 参数校验完全遵循 Linux 约定：`name` 长度限制为 `MEMFD_NAME_MAX=249`，不允许包含 `'/'`；`flags` 仅允许 `MFD_CLOEXEC (0x1)` 与 `MFD_ALLOW_SEALING (0x2)`，其余返回 `EINVAL`。        
    - `MFD_CLOEXEC` 通过在进程的 `_ofile->_fl_cloexec[fd]` 上打标记实现。
- memfd 的内部表示   
    - 每个 memfd 文件对象对应一个 `fs::file` 结构，其中：        
        - `_seals`：一个 32 位位掩码，保存 `F_SEAL_*` 标志。            
        - `_sealing_allowed`：布尔值，标记是否允许后续添加 seal（由 `MFD_ALLOW_SEALING` 控制）。            
    - 若创建时未允许 sealing，则内核直接写入 `F_SEAL_SEAL`，表示该 memfd 已被完全封印。
==== memfd 的 sealing 支持
F7LY 对 `fcntl(F_ADD_SEALS/F_GET_SEALS)` 的支持与 Linux 接轨，但内部逻辑专门围绕比赛内核的 VFS 层实现：
- fcntl 路径   
    - 仅当 `f->_path_name` 以 `memfd:` 开头时允许执行 sealing。        
    - 如果 `_sealing_allowed == false`，则禁止添加新 seal，返回 `EPERM`。        
    - 添加 `F_SEAL_WRITE` 前，会检查该 memfd 是否已在当前进程中以 `MAP_SHARED|PROT_WRITE` 方式映射，若存在则返回 `EBUSY`。        
- 系统调用路径上的强制检查    
    - `sys_mmap`：禁止对带有 `F_SEAL_WRITE` 的 memfd 建立写共享映射。        
    - `sys_mprotect`：禁止对已映射区域新增写权限。        
    - `sys_ftruncate` / `sys_fallocate`：分别检查 `F_SEAL_SHRINK` 与 `F_SEAL_GROW`，若冲突则拒绝。        
    - `normal_file::write`：写入时检查 `_seals & F_SEAL_WRITE`，若被封印则返回 `EPERM`。      

#text()[#h(2em)]这些检查点保证了 seal 的一致性，使得 memfd 在 F7LY 内核中具备了与 Linux 接近的语义。

=== 管道机制
管道（Pipe）是操作系统提供的一种进程间通信（IPC）机制，允许一个进程的输出直接作为另一个进程的输入。F7LY内核实现了符合POSIX标准的管道机制，支持匿名管道和命名管道（FIFO）。
==== 管道的基本实现
F7LY的管道实现基于虚拟文件系统（VFS），通过`pipe_file`类来管理管道的读写操作。管道的核心数据结构包括：
```cpp
	class pipe_file : public file
	{
	private:
		uint64 _off = 0;
		proc::ipc::Pipe *_pipe;
		bool is_write = false;//读端还是写端
		eastl::string _fifo_path; // 用于 FIFO 文件的路径跟踪
		int _pipe_flags = 0; // 管道标志，默认为0
  public:
		void set_fifo_path(const eastl::string& path) { _fifo_path = path; }
    long read(uint64 buf, size_t len, long off, bool upgrade) override
    long write(uint64 buf, size_t len, long off, bool upgrade) override 
    //其余字段省略
  }
```
#text()[#h(2em)]管道文件类`pipe_file`继承自`file`类，包含一个`_pipe`指针，指向实际的管道数据结构。该类实现了读写操作，并支持FIFO文件的路径跟踪。

```cpp
		class Pipe
		{
			friend ProcessManager;
			friend class fs::FifoManager; // 允许 FifoManager 访问私有成员
		private:
			SpinLock _lock;
			// 使用动态分配的循环缓冲区
			uint8 *_buffer;
			uint32 _pipe_size; // 动态管道大小
			uint32 _head;  // 读取位置
			uint32 _tail;  // 写入位置
			uint32 _count; // 当前数据量
			bool _read_is_open;
			bool _write_is_open;
			bool _nonblock; // 非阻塞模式标志
			uint8 _read_sleep;
			uint8 _write_sleep;
			int pipe_flags; // 管道标志
    public:
      int write( uint64 addr, int n );
			int write_in_kernel( uint64 addr, int n );
			int read( uint64 addr, int n );
			int alloc( fs::pipe_file * &f0, fs::pipe_file * &f1);
			void close( bool is_write );
      }
```
#text()[#h(2em)]管道的读写操作通过`read`和`write`方法实现，支持阻塞和非阻塞模式。基本实现方式是通过`Pipe`类来管理管道的缓冲区和读写指针。
#figure(
  image("fig/管道.png", width: 50%),
  caption: [管道基本实现],
) <fig:pipe-basic-implementation>
==== 管道管理器
为记录有名管道的创建和销毁，F7LY实现了一个管道管理器`FifoManager`，用于管理所有有名管道的生命周期。该管理器维护一个unordered_map容器，并提供创建、删除和查找管道的接口。
基础的管道信息结构体由读者写者计数和管道本身组成:
```cpp
struct FifoInfo {
    proc::ipc::Pipe *pipe;
    int reader_count;
    int writer_count;
    
    FifoInfo() : pipe(nullptr), reader_count(0), writer_count(0) {}
    FifoInfo(proc::ipc::Pipe *p) : pipe(p), reader_count(0), writer_count(0) {}
};
```
#text()[#h(2em)]在 `pipe_file`类中有一个`_fifo_path`字段，用于跟踪有名管道的路径。通过该路径，F7LY能够在文件系统中创建和管理有名管道。
`FifoManager`类提供了以下关键方法：
```cpp
    proc::ipc::Pipe* get_or_create_fifo(const eastl::string& path);
    bool open_fifo(const eastl::string& path, bool is_writer);
    void close_fifo(const eastl::string& path, bool is_writer);
    bool has_readers(const eastl::string& path);
    bool has_writers(const eastl::string& path);
    FifoInfo get_fifo_info(const eastl::string& path);
```
#figure(
  image("fig/fifomanager.png", width: 75%),
  caption: [有名管道管理器],
) <fig:fifo-manager>
#text()[#h(2em)]需要创建管道时，调用`get_or_create_fifo`方法，该方法会检查是否已经存在同名的管道，如果存在则返回对应的管道指针，否则创建新的管道并返回。通过`open_fifo`和`close_fifo`方法，F7LY能够管理管道的打开和关闭操作，并维护读者和写者计数。


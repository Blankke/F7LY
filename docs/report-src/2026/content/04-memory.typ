= 内存管理

== 内核地址空间

=== 物理内存发现与布局

内核通过设备树（DTB）获知物理内存的分布。DTB 由固件（OpenSBI / EDK2）在启动时经 `$a1` 寄存器传入，`PhysicalMemoryManager::init()` 解析其中 `/memory` 节点的 `reg` 属性，得到每段可用 RAM 的基地址与大小。

RISC-V 的 qemu virt 平台通常只有单段连续 RAM。内核以 DTB 所在地址的页对齐下界作为物理内存安全上界，保护 DTB 不被 Buddy 分配器覆盖。可用内存按约 2:1 比例划分为低端 Buddy 页分配区和高端内核堆 + SHM 共享内存区。

LoongArch 的情况更复杂。以 `-m 1G` 为例，物理地址空间被拆为低端 RAM（含内核镜像）和高端 RAM（`0x90000000` 以上），中间夹着 PCI 配置空间等空洞。PMM 首先定位包含内核 `end` 符号的 region 作为 Buddy 管理区，然后从高地址向低地址搜索第一块足够大的独立 region 整段划给内核堆和 SHM（Split Heap）。这样低端区全留给用户页分配，高端区独立承载内核堆，绕开了物理地址空洞。

LoongArch 通过 DMWIN（Direct Mapping Window）实现物理地址的直接访问，无需页表翻译。DMWIN0 覆盖 MMIO 区域，DMWIN1 覆盖内存区域。内核提供 `to_vir` / `to_phy` / `to_io` 等地址转换宏，释放物理页时通过 `normalize_managed_page_addr` 统一折算防止错误地址塞回 Buddy。


#figure(
  image("fig/内核地址空间.png", width: 70%),
  caption: [内核地址空间示意图],
) <fig:mem-kernel-address-space>


=== 内核内存分配器

内核采用三层分配体系。最底层是 Buddy System:以 4KB 页为单位的伙伴分配器，内部用扁平数组实现的完全二叉树管理空闲页块，节点标记为空闲、已用、分裂或全满四种状态，分配时自顶向下二分搜索，释放时递归合并空闲伙伴。
#figure(
  image("fig/buddy-allocator.png", width: 70%),
  caption: [buddy-allocator],
) <fig:mem-buddy-allocator>

中间层 `PhysicalMemoryManager`（`k_pmm`）封装 Buddy，从内核 `end` 符号处启动，预留元数据后初始化页池，对外提供 `alloc_page` / `free_page` 接口。全局数组记录每个物理页的 16 位引用计数——`alloc_page` 初始设为 1，`fork` 时 `retain_page` 递增，`free_page` 递减到零才归还 Buddy，这是 COW 的基础。
#figure(
  image("fig/物理内存管理.png", width: 70%),
  caption: [物理内存管理示意图],
) <fig:mem-physical-memory-manager>

上层 `HeapMemoryManager`（`k_hmm`）解决 Buddy 只能按页分配的问题。它在内核堆区域上叠加两层架构：粗粒度层用 Buddy 申请 32 页大块，细粒度层用 liballoc 算法在块内做任意大小的 `malloc` / `free`。内核所有 `new` / `delete` 经重载统一路由到 `k_hmm`。

#figure(
  image("fig/堆空间管理.png", width: 70%),
  caption: [堆空间管理示意图],
) <fig:mem-heap-memory-manager>


此外，`SlabAllocator` 为 16~256 字节的高频固定大小对象提供 O(1) 缓存，维护空闲、部分满、全满三个链表，作为可选优化保留。

=== 双架构页表差异

两个架构在内核地址空间的映射方式上有本质差异。RISC-V 采用 Sv39 三级页表的恒等映射，内核链接在 `0x80200000`，通过页表访问所有物理内存和设备 MMIO。LoongArch 凭借 DMWIN 直映窗口直接访问物理地址，内核链接在 `0x9000000080000000`，页表更多服务于用户态地址空间。

内核页表由 `kvmmake()` 创建。RISC-V 侧依次映射设备 MMIO、内核 text（只读可执行）/ data（可读写）、DTB 页面、TRAMPOLINE 跳板页和内核堆区。LoongArch 侧分情况映射低端连续区（去掉 DMWIN 掩码后的虚拟地址）和高端 Heap 区，已由 DMWIN 覆盖的区域不再重复走页表。

每个进程的内核栈位于 `TRAPFRAME` 下方，占 2 页加 1 页未映射 guard page。栈溢出触及 guard page 会触发异常而非静默破坏相邻数据。

== 用户地址空间

=== 用户地址空间布局

F7LY-OS 的用户进程地址空间从低地址到高地址依次排布，所有区域均以 VMA 描述，记录在 `ProcessMemoryManager` 的 `VMASpace` 中。

#figure(
  image("fig/用户地址空间.png", width: 70%),
  caption: [用户地址空间示意图],
) <fig:mem-user-address-space>

*NULL guard*:低 64KB（地址 `0x0 ~ 0x10000`）保持未映射状态，用于捕获空指针和 NULL 偏移解引用，触发缺页异常后向进程投递 `SIGSEGV`。

*ELF 程序段*:静态链接或主可执行文件的 `PT_LOAD` 段按 ELF 声明的 `p_vaddr` 装载。对于 PIE（位置无关可执行文件），内核在首个 `PT_LOAD` 的基址上加载各段，段权限严格按 `p_flags` 中的 R/W/X 映射。每个段注册为一个 `VmAreaKind::ElfLoad` 类型的 VMA。

*动态链接器段*:若 ELF 包含 `PT_INTERP` 头（如 `/lib/ld-musl-riscv64-sf.so.1`），内核额外加载解释器 ELF 的各 `PT_LOAD` 段。解释器的装载基址由 `highest_addr - interp_min_vaddr` 按 `p_align` 对齐计算——LoongArch 的 glibc 解释器要求 16KB 对齐，RISC-V 的 musl 解释器使用 4KB 对齐。解释器各段注册为 `VmAreaKind::InterpreterLoad` 类型。

*用户栈*:`execve` 在主程序和解释器的所有段装载完毕后，紧接最高已用地址上方放置用户栈：先留 1 页 guard page，再分配 256 页（1 MB）栈空间，权限 `R | W`，`MAP_GROWSDOWN` 标志允许栈在缺页时自动向下扩展。栈 VMA 类型为 `VmAreaKind::UserStack`，增长策略为 `VmGrowPolicy::Down`。线程栈则由 `clone` / `mmap` 在 mmap 区域另行分配，不与此主栈共享 VMA。

*堆*:`brk` 堆初始化在用户栈之上（`PGROUNDUP(highest_addr)`），类型 `VmAreaKind::Heap`，增长策略 `VmGrowPolicy::Up`。`sbrk` / `brk` 系统调用通过 `grow_heap()` 向上扩展堆，上限为 `TRAPFRAME - 64KB`，防止堆无限增长冲入内核区。

*mmap 区域*:堆上方预留 64KB 保护间隔（`k_mmap_guard_gap`）后为 mmap 分配区，最小基址为 `0x10000000`（`k_mmap_min_base`）。`mmap_cursor` 从此处向上搜索空闲虚拟地址区间，通过 `VmaMapleTree::find_gap` 在 O(log N) 内定位合适位置。此区域承载匿名映射（`MAP_ANONYMOUS`）、文件映射（`MAP_FILE`）、共享内存附加（`SysvShm`）以及线程栈。

*顶端特殊页*:

- `TRAMPOLINE`：内核态与用户态切换的跳板代码页，权限 `R | X`，同时映射到内核和每个用户进程的页表中。
- `SIG_TRAMPOLINE`：信号处理返回的跳板页，`sigreturn` 通过此处安全恢复被信号打断前的寄存器现场。
- `TRAPFRAME`：进程陷阱帧页面。用户态陷入内核时 `uservec` 将通用寄存器保存到此页，返回用户态时 `userret` 从此页恢复。

这三个页面由 `create_pagetable()` 在进程创建时统一映射，不注册为 VMA——它们不属于用户可控的 mmap / munmap 范围。



=== 进程内存管理器（ProcessMemoryManager）

`ProcessMemoryManager`（以下简称 `mm`）是一个进程全部用户态内存资源的管家。每个 `Pcb` 持有一个 `mm` 指针，`mm` 内部维护进程页表、程序段描述、`brk` 堆的起止地址（`heap_start` / `heap_end`）、mmap 自动选址用的 `mmap_cursor`，并将全部 VMA 的生命周期统一交给 `VMASpace` 管理。

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
        uint64 mmap_cursor;

        // 页表管理
        mem::PageTable pagetable;

        // 地址空间元数据
        VMASpace vm_space;

        // 共享标志
        bool shared_vm;

    private:
        // 内存大小
        uint64 total_memory_size;

    public:
        //...各种公共方法
    }
```

`mm` 用原子引用计数区分“共享”和“独立”两种地址空间模型。创建一个进程时，`fork` 走 `clone_for_fork()`——把父进程的页表和 VMA 完整拷贝一份，物理页降级为 COW，父子从此各自独立，一方的 `mmap` 或 `brk` 不影响另一方。创建一个线程时，`clone` 带 `CLONE_VM` 标志走 `share_for_thread()`——只把 `mm` 的引用计数 $+1$，指针直接共享，任何线程调用 `brk` 或 `mmap` 其他线程都能看到。`execve` 则是彻底换掉地址空间：创建一个全新的 `mm`，装好新 ELF 后原子替换旧的，旧 `mm` 引用计数减 $1$。进程退出时，`exit` 调用 `put()` 递减引用计数，当计数归零时由最后一个退出的线程执行 `free_all_memory()`，按 VMA $→$ 程序段 $→$ 堆 $→$ 页表的顺序释放所有内存。

VMA 和堆的元数据操作受 `SleepLock`（`memory_lock`）保护。之所以用睡眠锁而非自旋锁，是因为 `munmap` 等操作可能触发文件映射脏页写回，进而进入 ext4 和 virtio 的磁盘 I/O 路径——自旋锁在此期间会导致死锁。

=== VMA 组织

VMA（Virtual Memory Area）是 Linux 描述进程地址空间的核心抽象——一段连续的虚拟地址区间，拥有统一的权限和同一个数据来源。F7LY 采用“描述符 → 容器与索引 → 后端对象”的三层组织方式来管理 VMA。

#figure(
  image("fig/vma.png", width: 70%),
  caption: [vma示意图],
) <fig:mem-vma-architecture>


==== VMA 描述符（VmArea）

一个 VMA 描述用户地址空间中一段连续区间，核心在于：

VMA 按来源分为六种类型：ELF 装载的程序段、动态链接器段、堆、用户栈、mmap 映射和SysV 共享内存。类型决定了它的增长方式和清理顺序——比如堆只向上扩展，栈只向下扩展且在`execve` 时不必写回。

VMA 记录了读/写/执行权限以及映射方式是私有还是共享。私有映射的写入触发 COW 拆页，只影响当前进程；共享映射的写入会最终写回文件或对其他进程可见。

VMA 不自己分配物理页，而是指向一个后端对象。匿名映射的后端在访问时按需产生全零页，文件映射的后端在缺页时读磁盘，共享内存的后端从共享段取页。这样同一个文件可以被多个进程各自建立一份 VMA，而底层共享同一个文件缓存对象。

对私有文件映射或 `fork` 后的共享页，一旦某个页面被写入，COW 机制会分配一个新物理页存放私有副本。VMA 内部记录这些已拆页的页号，后续访问直接使用私有副本，不再触发缺页。

==== VMASpace 与 Maple Tree 索引

`VMASpace` 是 VMA 的集中管理容器，内部同时维护两份数据：一份 `eastl::list` 链表持有所有 VMA 对象本身，另一份 `VmaMapleTree` B+Tree 按地址对它们做索引。链表用于全量遍历，例如 `free_all_memory` 清理和 `/proc/self/maps` 导出；树索引用于按地址的精确查找和区间搜索。

`VmaMapleTree` 的设计参考了 Linux 的 Maple Tree。它是一棵纯索引树——树节点只存 VMA 的指针和排序键（`addr`），不拥有数据的所有权。叶子节点之间通过双向链表连接，使得给定一个地址时，既可以通过树快速定位覆盖它的 VMA，也可以从当前位置沿叶链顺序遍历后续区间，而无需回溯整棵树。

快速区间索引对 `mmap` 同样关键。`mmap` 需要在两个已有 VMA 之间找到足够大的空隙。`VmaMapleTree` 沿叶子链表顺序扫描相邻 VMA 之间的间隔，给出第一个满足对齐和大小要求的空闲区间；整个过程只遍历实际存在的 VMA 条目，不会被稀疏地址空间中的空洞拖慢。

插入和删除时，树索引与链表双写。`mprotect` 等操作可能将一个 VMA 拆成两段或三段，`munmap` 可能从中间打洞，此时 `VMASpace` 先更新链表中的 VMA 对象，再同步更新树索引。部分批量操作后会调用 `rebuild_vma_index()` 从链表全量重建树索引，用一次性重建替代多次碎片化更新带来的平衡开销。

==== VMObject 后端抽象

`VmObject` 的引入将这些数据来源的差异统一到一个抽象基类背后，对外只暴露一个接口：给定 VMA 区间内的某个页序号和访问类型（读/写/执行），返回一个可安装到页表中的物理页。

`VmObject` 有三个子类，各自实现不同的 `prepare_page` 逻辑：

*匿名对象（AnonVmObject）*：用于 `MAP_ANONYMOUS`、堆和栈。首次访问时分配一个全零物理页，再次访问同一页时直接返回已分配的页。`fork` 后父子共享同一个匿名对象，各自的 VMA 通过 COW 机制隔离写入。

*文件对象（FileVmObject）*：用于 `mmap` 文件映射。缺页时从磁盘读取文件内容到物理页中。对于 `MAP_SHARED` 的文件映射，相同文件在同一内核内共享同一个 `FileVmObject`（通过文件路径的缓存键去重），使得多个进程的共享文件映射真正看到同一份物理页，一方的写入对另一方立即可见。`MAP_PRIVATE` 的文件映射则各自持有独立的对象实例，写入触发 COW 拆页后记录到 VMA 的私有页表中，不影响磁盘原文件和其他进程。进程退出或 `munmap` 时，共享对象的脏页通过 `sync_area_range` 写回磁盘。

*SysV 共享内存对象（SysvShmVmObject）*：用于 `shmat` 附加的共享内存段。内部持有段的元数据（key、大小、权限、创建者等），`prepare_page` 从共享段的物理页数组中取页。与文件对象类似，多个进程附加同一段时共享同一个对象实例。

`VmObject` 内部维护一个 `source_pages_` 缓存——从逻辑页序号到物理页地址的映射。这个缓存的意义在于 `fork`：当父进程通过 `VmObject` 分配了一系列物理页后，`fork` 创建的子进程通过 COW 共享这些页，但子进程的 VMA 指向同一个 `VmObject` 实例（对象引用计数 $+1$）。子进程首次读缺页时，`VmObject` 直接从 `source_pages_` 命中已有物理页，不必重新分配，只需在页表中安装 COW 映射。

对象的生命周期由原子引用计数管理。VMA 创建时 `get()`，销毁时 `put()`。当所有引用释放后，匿名对象和 SysV 对象直接释放缓存的物理页，文件对象则先写回脏页再释放。

=== 缺页异常处理

缺页异常是用户地址空间中惰性分配和 COW 的驱动入口。当用户态访问一个没有合法页表映射的虚拟地址时，硬件触发异常，经 `usertrap` 分发到 `mmap_handler`，最终由 `ProcessMemoryManager::fault_page` 按照一个固定的优先级链处理。

*COW 优先*:缺页后第一步是检查该地址是否命中了 COW 页面——即 PTE 存在且带有 `PTE_COW` 标志，且当前访问是写入。如果是，调用 `resolve_cow_page` 分配一个新物理页，将原页内容复制过来，把新旧 PTE 都恢复为可写并清除 COW 标记。这一步必须在 VMA 查找之前完成，因为 COW 页在页表中已经有一个“只读 + COW”的 PTE，如果直接去查 VMA 做惰性分配会误判为未映射。

*惰性分配*:如果 PTE 根本不存在（真正的缺页），通过 `VMASpace` 查找覆盖该地址的 VMA。找到后，检查访问类型是否与 VMA 声明的权限匹配——例如对只读映射写入会直接返回失败。权限检查通过后，调用 VMA 绑定的 `VmObject::prepare_page` 获取物理页，再根据 VMA 的 `prot` 和 `flags` 构建 PTE 标志位（私有映射写入时对匿名/文件对象标记 COW，共享映射写入时直接标可写），安装到页表中。

*栈自动扩展*:如果缺页地址没有命中任何 VMA，但紧邻用户栈 VMA 的下方（栈向下增长方向），且地址高于栈的 guard page 上界，则内核自动将栈 VMA 的起始地址向下扩展到缺页地址，然后重新走惰性分配流程。这种自动扩展使得 `MAP_GROWSDOWN` 栈可以按需增大，而非一次性分配全部 `max_len` 的物理页。

*TLB 残留重试（仅 LoongArch）*:LoongArch 的 TLB 由软件管理，可能出现 PTE 已正确安装但 TLB 中仍残留无效表项的情况。在进入 VMA 查找之前，若 PTE 已存在、有效且权限与当前访问匹配，内核执行一次 `invtlb` 精准失效后直接返回重试用户指令，不将其误判为缺页失败。

以上任一环节失败，进程收到 `SIGSEGV`，内核在信号帧中记录 `si_addr` 为触发异常的虚拟地址，供用户态调试器或信号处理函数定位。

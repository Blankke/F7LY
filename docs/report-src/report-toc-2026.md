# F7LY-OS 2026 年设计文档目录

---

## 第一章　内核概述

### 1.1　内核介绍

### 1.2 主要工作
#### 1.2.1 参考的代码
#### 1.2.2 我们的工作
内存管理
进程管理
文件系统
进程间通信
线程管理
系统调用完善
硬件移植

### 1.3　整体架构与项目结构
图1-1：F7LY-OS 整体架构图
> user变成shell和initcode区域
> 网络的部分扩写
> 添加设备树DTB，fs部分扩写
> SHM添加了VMobject需要扩写


  - boot 系统启动模块 — 负责内核的启动流程，包含 RISC-V 和 LoongArch
  两种架构的启动代码，实现从 bootloader（OpenSBI / 直接启动）到内核 main()
  函数的跳转，完成栈设置、页表初始化、硬件环境配置等早期初始化工作。
  - hal 硬件抽象层 — 提供跨架构的硬件抽象，封装 CPU 相关操作（CSR
  寄存器读写、页表操作）和上下文切换（swtch.S），为上层模块屏蔽 RISC-V 和 LoongArch
  的底层差异，包括 SBI ecall 接口（RISC-V）、DMWIN
  窗口配置（LoongArch）、中断控制器抽象等。
  - libs 内核库模块 — 提供内核所需的基础库函数，包括格式化打印输出（Printer
  类，支持日志级别/颜色/分组过滤）、字符串操作、C++ ABI
  运行时支持（__cxx_abi）、模板算法、排序、EASTL 适配器、全局 operator new/delete
  等，为内核其他模块提供 freestanding 环境下的基础服务。
  - mem 内存管理模块 — 实现完整的内存管理体系，包含物理内存管理器（伙伴系统 Buddy System
  算法，alloc_page/free_page）、虚拟内存管理器（SV39 / LA64
  页表管理，地址空间映射）、堆内存管理器（kmalloc/kfree）、SLAB
  分配器（小对象高效分配），以及 COW（Copy-on-Write）fork 的页引用计数机制。
  - trap 中断与异常处理模块 — 处理硬件中断、异常和系统调用，实现中断向量表设置（kernelvec
  .S/uservec.S）、异常分发流程、时钟中断管理，支持 RISC-V PLIC 和 LoongArch APIC + ExtIOI
  双架构的不同中断机制，包含 TLB refill 异常处理（LoongArch 软件走表）。
  - proc 进程管理模块 — 实现进程创建、调度、同步等核心功能，包含进程管理器（fork/exec/exi
  t/wait4/clone3）、调度器（Round-Robin + 优先级）、POSIX
  信号处理（sigaction/sigprocmask/sigreturn）、Futex
  快速用户态互斥锁、管道通信（pipe/FIFO）、POSIX 定时器、进程组/会话管理、VMA（Maple Tree
  虚拟内存区管理）等，支持多进程并发执行。
  - sys 系统调用模块 — 实现 ~800 个 Linux ABI
  兼容系统调用，提供用户态程序与内核的交互接口，包含 2048 槽的系统调用分发表、参数传递与
  校验、权限检查等功能，覆盖文件操作、进程控制、内存管理、信号、socket
  网络、定时器、共享内存等全部 Linux asm-generic 调用约定。
  - fs 文件系统模块 — 实现 VFS（虚拟文件系统）层，支持多种文件系统包括
  ext4（lwext4，含日志/扩展树/xattr/目录索引）、FAT32、ramfs/initrd
  等，提供统一的文件操作接口（file 抽象类），包含缓冲区缓存（bio）、目录项缓存、inode
  管理、路径解析、挂载管理（bind mount），以及 /proc
  虚拟文件系统和设备文件、管道文件、socket 文件等多种文件类型。
  - devs 设备管理模块 — 实现统一的设备抽象层，包含字符设备（CharDevice）、块设备（BlockDe
  vice）、流设备（StreamDevice）等抽象类，提供设备管理器（256
  槽设备表）进行设备注册和按名查找，支持 UART 串口、Console
  控制台（stdin/stdout/stderr，含 termios 行规程）、VirtIO 磁盘、ramdisk、loop
  设备等硬件/虚拟设备。
  - shm 共享内存模块 — 实现 SysV
  进程间共享内存机制（shmget/shmat/shmdt/shmctl），支持匿名共享内存和有名共享内存，提供高
  效的跨进程数据交换能力，包含共享内存分配、映射、同步等功能。
  - tm 时间管理模块 — 提供时间和定时器相关服务，实现定时器管理器（tick 时钟、sleep
  睡眠、alarm 闹钟）、时间接口（gettimeofday/clock_gettime）、NTP
  时间调整（adjtimex），为进程调度（时间片）和系统调用（超时阻塞）提供时间基础服务。
  - net 网络模块 — 实现网络协议栈的基础框架，包含 ONPS（Open-NPStack）轻量级 TCP/IP
  协议栈集成（Ethernet/ARP/IPv4/ICMP/TCP/UDP/BSD Socket API）、VirtIO Net 网络驱动（支持
  RISC-V MMIO 和 LoongArch PCI 两种后端）、VFS socket 文件抽象（AF_INET + AF_UNIX + 本地
  loopback 快速路径），以及 socket 兼容层（Linux ioctl 兼容，SIOCGIFCONF 等）。
#### 1.3.1　分层架构与模块职责

**2026 年改为六层分层图：**
1. 架构启动、HAL、trap 与设备发现
2. 内存、进程、线程与 IPC
3. Linux ABI 与系统调用
4. VFS、文件对象、ext4/FAT32 与块设备
5. socket、loopback 与 ONPS/VirtIO Net
6. 用户程序、shell、回归与评测工具

#### 1.3.2　项目目录结构

```
├── build/
├── kernel/
│   ├── boot/          — RISC-V / LoongArch 启动
│   ├── devs/          — 设备驱动（UART、DTB、console、loop、virtio-blk 队列）
│   ├── fs/
│   │   ├── drivers/   — 统一 virtio-blk 驱动 + priority-borrow 调度器  [→ 6bdca8c]
│   │   ├── lwext4/    — ext4 文件系统
│   │   ├── fat32/     — FAT32 数据盘（降级为辅助文件系统）  [→ 5c0c055]
│   │   └── vfs/
│   │       └── file/  — 文件对象派生类（+ epoll_file）  [→ de2f2c9]
│   ├── hal/           — 硬件抽象层（CPU、RISC-V CSR/LA CSR、地址窗口转换）
│   ├── libs/          — 基础库（手写替代 EASTL）  [→ bb56d82]
│   ├── link/          — 链接脚本
│   ├── mem/           — 内存管理（伙伴系统、slab、页表、VMA）
│   ├── shm/           — 共享内存（System V + POSIX）
│   ├── proc/          — 进程管理（PCB、调度器、信号、futex、POSIX timer）
│   ├── sys/           — 系统调用（syscall_abi / sysio / sysproc 三分模块）  [→ 5319d08]
│   ├── tm/            — 时间管理（+ TimexController）  [→ 5319d08]
│   ├── trap/          — 异常与中断（+ LA TLB/LL-SC 探针）  [→ 3c0be3c]
│   └── capability/    — 权限管理  [→ 5319d08]
├── user/              — 用户态程序（+ llsc_exec_probe 原子探针）  [→ 3c0be3c]
├── tools/             — LTP 工具链（runner/parser/ranker）  [→ 6356a47]
├── debug/             — GDB 脚本  [→ bb56d82]
├── scripts/           — mount/rootfs 脚本  [→ bb56d82]
├── scoreboard/        — 四组合评测状态  [→ c858ac1]
├── agent_docs/        — 架构与协作文档  [→ c858ac1]
└── Makefile           — 双模式构建（evaluation / shell）  [→ ef0e617]
```

---

## 第二章　机器启动

> 旧报告对应位置：旧 §2.1（启动流程）+ 旧 §2.2（中断与异常管理）的前半部分。
> 最大变化：LA 从 ACPI → DTB；新增 DTB 解析器独立成节；新增 LA trap/TLB 独立成节。

> 重绘，添加DTB的注册等我们新增的逻辑。说明启动的时候初始化的东西，如trap_mgr.inithart()等

### 2.1　RISC-V 启动流程

 2.1.1 entry.S

  - OpenSBI 传入 hartid (a0) + DTB 地址 (a1)，跳转 _entry
  - 多核栈分配：sp = stack0 + 4096 × (hartid + 1)，每核 8KB
  - 跳转 start()
  - 变化：无实质变化，但 DTB 地址在 2026 年从"传入但不用"变为"被 PMM真正利用"（ac2b88d）

  2.1.2 start.cc

  - satp = 0 关 MMU  关闭地址翻译，等 main()里建好页表后再开启。
  - stvec = trap_loop 兜底  发生异常，死循环
  - tp = hartid     把 tp 寄存器的值读出来，快速获取当前 CPU 编号
  - 调用 main(hartid, dtb_entry)
变化：
  sie 早期初始化代码已注释掉，中断使能统一交给 trap_mgr.inithart()，即“谁负责 trap 体系，谁负责开中断。”       5c0c055 
  dtb_entry 被 PMM 用于安全上限截断（usable_top =  PGROUNDDOWN(k_dtb_addr)）  2.1.1 ，保护 DTB数据不被覆盖        ac2b88d                                
  控制台输入改为全走 SBI，不再直读 MMIO UART    内核 → ecall 调用 SBI → OpenSBI → 操作 UART 硬件     好处是内核不再依赖具体 UART 硬件型号，移植性更好             5727aaa 
  

  2.1.3 主线：main() 四阶段  
 > 简单的流程图，说明main初始化的阶段，包括main.cc里面和forkret之后的fs_init，net_init等。 

  阶段一 —— 打印 / trap / 中断：初始化的是最基础的服务
  - DtbManager::init(dtb_addr) 地址规范化（新增）
  - k_printer.init() → trap_mgr.init()/inithart() 把 stvec 从之前的死循环 trap_loop 改成真正的 trap
  入口 uservec，配置中断使能等→ plic_mgr.init()/inithart()
   
  阶段二 —— 内存管理：
  - k_pm.init() → k_pmm.init() → k_vmm.init() → k_hmm.init() → k_smm.init() →
  SlabAllocator::init() 有前后依赖关系
  - 变化：PMM 内存边界从固定 PHYSTOP → DTB 动态获取（详见 §2.3）  

  阶段三 —— 设备 / 文件系统 / 首个进程：初始化的是用户程序运行需要的所有基础设施
  - 注册 stdin/stdout/stderr → 定时器 → 系统调用表（系统调用号到处理函数） → user_init()（创建第一个用户进程）
  - 统一 virtio-blk → ext4 初始化 → VFS 虚拟文件系统 → FIFO → loop
  - 变化：virtio_disk_init() 从架构专用驱动 → 统一 virtio-blk 框架（详见
  §5.1）；initcode 分评测/Shell 双模式（详见 §9.1）

  阶段四 —— 启动调度器：开始执行 initcode
  - k_scheduler.init() → k_scheduler.start_schedule()（永不返回：  从此以后，main() 不再执行。整个操作系统由中断驱动：时钟中断触发调度、系统调用
  提供服务、外设中断处理 I/O。如果 start_schedule()
  返回（说明没有进程可调度了），就调用 sbi_shutdown() 关机。）



### 2.2　LoongArch 启动流程
2.2.1 入口：entry.S
  - QEMU/EDK2 固件将 DTB 物理地址放在 $a1，跳转 _entry
  - move $s0, $a1 保存 DTB 地址到 callee-saved 寄存器
  - 设置 DMWIN0（设备访问窗口）、DMWIN1（内存访问窗口）
  - CSR 初始化：CRMD（PLV=0, IE=0, PG=1）、PRMD、EUEN（FPE=1） 设置了 CPU 的运行状态
  - 栈分配：sp = boot_stack + 4096 × (CPUID + 1)
  - hartid 从 CSR_CPUID 动态读取（老版本硬编码，已删除）
  - tp = CPUID
  - bl main 直接跳转 —— LA 没有 start() 薄层

2.2.2 main() 四阶段（仅标注 LA 与 RISC-V 的差异）

  - 阶段一：find_dtb_and_initrd() 暴力搜索 DTB + initrd（RISC-V不搜），apic_init() + extioi_init() 两级中断控制器（RISC-V 只有 PLIC）
  - 阶段二：split heap —— DTB 多段内存时，低端做 buddy，高端整段做 heap+shm
  - 阶段三：virtio_probe() 先做 PCI 枚举（RISC-V 不需要）
  - 阶段四：和 RISC-V 完全一致


### 2.3 双架构支持特色
#### 2.3.1 启动完成标志

## 第三章 中断管理器
  3.1 整体设计

  - 对象化 trap_manager，全局实例 trap_mgr
  - 统一接口：init()、inithart()、devintr()、usertrap()、usertrapret()、kerneltrap()
  - 底层按架构分文件夹封装
  - 包装函数（wrap_ 前缀）设置中断入口
  - machine_trap() 已删除 （LA重构后，三个入口各有各的地址）

  3.2 RISC-V 中断处理路径

  - PLIC 单层中断控制器，外设中断汇总后按优先级分发各核
  - 入口 uservec.S → 保存寄存器 → 切换内核页表 → usertrap() RISC-V uservec.S 新增 FPU 现场保存（337f4a7）
  - 返回 usertrapret() → userret → 恢复寄存器 → sret
  - 内核态异常走 kernelvec.S → kerneltrap()
  - 控制台输入改走 SBI（5727aaa）

  3.3 LoongArch 中断处理路径

  - APIC（核本地）+ ExtIOI（外部路由）两级中断控制器
  - 入口 kernelvec.S → 保存寄存器 → 从 CSR 重取 CPUID → usertrap() 或 kerneltrap()
  - 三个异常入口：kernelvec（常规）、handle_tlbr（TLB 重填）、handle_merr（机器错误）
  - 返回 usertrapret() → trapframe 原子 remap → ertn
  - TICLR 直接写清除位
  - UART0 中断分发（b66f29a）
  - TLB 管理：精准失效（invtlb 0x6）、残留重试、TLB 探针
  - 入口 kernelvec.S → 保存寄存器 → 从 CSR 重取 CPUID → usertrap() 或 kerneltrap()
  - 三个异常入口：kernelvec（常规）、handle_tlbr（TLB 重填）、handle_merr（机器错误）
  - 返回 usertrapret() → trapframe 原子 remap → ertn
  - TICLR 直接写清除位
  - UART0 中断分发（b66f29a）
  - TLB 管理：精准失效（invtlb 0x6）、残留重试、TLB 探针

  3.4 陷阱分发逻辑

  - usertrap() 确认来自用户态 → 读 ecode/esubcode 区分陷阱类型：
    - 系统调用 → invoke_syscaller()
    - 缺页 → COW → TLB 残留重试 → mmap_handler → 失败送 SIGSEGV
    - 地址错误 → SIGSEGV
    - 设备中断 → devintr()
  - devintr() 内：先确保 ecode==0，再按 pending bit 分发硬件中断 / timer 中断
  - 时钟中断 → timeslice++ → yield
  - 信号处理 → usertrapret()


---

## 第四章　内存管理

> 旧报告对应位置：旧 §2.3（内存管理，含虚拟内存与物理内存两个子节）。
> 最大变化：固定内存布局 → DTB 驱动；地址空间所有权统一到 ProcessMemoryManager；mmap/SHM 从旧 IPC 章移入。
 ---
  ### 4.1 地址空间

  #### 4.1.1 内核链接地址 
  图 4-1 内核地址空间布局


  - RISC-V：内核链接在 0x80200000，通过链接脚本 kernel/link/riscv/kernel.ld 指定，OpenSBI 加载到此处
  - LoongArch：内核链接在 0x9000000080000000（DMWIN1 直映窗口），通过链接脚本
  kernel/link/loongarch/kernel.ld 指定，EDK2 加载到对应物理地址后由 DMWIN1 访问

  #### 4.1.1 地址空间布局
图 4-2 用户地址空间布局

  ##### 4.1.2.1 内核地址空间布局

  - RISC-V：内核在物理低端，线性映射——虚拟地址 = 物理地址 + 固定偏移
  - LoongArch：DMWIN0/DMWIN1 双直映窗口覆盖全部物理内存和MMIO，内核通过窗口直接访问物理地址，不走页表
  - 变化：内核内存边界从固定 PHYSTOP 改为 DTB 动态确定（ac2b88d）
  - LA 新增 kernel_linear_top 字段跟踪内核线性区物理上界，知道有多少物理内存可用

  ##### 4.1.2.2 用户地址空间布局

  - 用户程序从 0x10000 开始（低 64KB 保留为 NULL guard）
  - ELF 各段（text/data/BSS）按 p_vaddr 装载，heap 在 BSS 后向上增长，stack在高地址向下增长
  - 动态链接时 loader（ld-musl-*.so 或 ld-linux-*.so）被映射到用户空间高地址
  - 变化：ProcessMemoryManager（699e042），统一管理页表、程序段、heap、VMA、mmap cursor和引用计数 

  ---
  ### 4.2 物理内存管理
图 4-3 物理内存管理

  #### 4.2.1 内核动态内存分配器（总览）

  - 三级分配体系：Buddy（页级，粗粒度）→ LibAllocator（HMM，任意大小）→SlabCache（固定大小，高频对象）
  - 重载 operator new / operator delete 对接 HMM
  - 变化：LA 侧堆区域可从 DTB 发现的独立高端 RAM 区分配（split heap），不再挤在低端（ac2b88d）

  #### 4.2.2 内核物理内存（伙伴系统 + PMM）

  - BuddySystem（PAGE_ORDER=10，PGNUM=1<<15）：伙伴算法管理 4KB 页的分配与回收
  - PhysicalMemoryManager 封装 buddy：alloc_page / free_page / kalloc / kfree
  - pa_start 从内核 end 标记开始，避开内核镜像和 DTB 数据区
  - 变化：PMM 初始化改为 DTB 驱动（ac2b88d）
    - LA：遍历 DTB 所有 memory region → 内核所在 region 做 buddy 区 + 独立高端 region
  整段做 heap/shm
    - RISC-V：DTB 地址作为 usable_top 安全上限
  - LA 新增 normalize_managed_page_addr()：释放物理页时统一折算为 DMWIN
  直映地址，防止错误页号塞回 buddy

图 3-4 堆空间管理

  #### 4.2.3 内核堆内存分配（HMM + Slab）

  - HeapMemoryManager（k_hmm）：LibAllocator 的 L'Major / L'Minor 两级分配算法，从 buddy批量拿页后切成任意大小
  - SlabCache：free_slabs / partial_slabs / full_slabs 三链表，为进程控制块、inode、file等高频对象提供固定大小的快速分配（O(1)）
  - 变化：堆区域支持从 DTB 发现的独立高端 RAM 区分配（ac2b88d） 还是这个东西

图 4-5 Buddy Allocator 分配示意图

  #### 4.2.4 地址空间管理（ProcessMemoryManager + VMA）

  - VirtualMemoryManager（k_vmm）：map_pages / unmap / kvmmap / uvmalloc / uvmdealloc /copy_in / copy_out，PageTable 隐藏 RISC-V Sv39 / LoongArch 4-level 差异
  - ProcessMemoryManager 管理每个进程的页表、ELF
  程序段、heap（heap_start/end、grow_heap/shrink_heap/brk）、VMA、mmap cursor
  - VMA 结构（addr/len/prot/flags/vfd/vfile/offset/max_len/is_expandable）：记录进程地址空间中每一段映射的属性
  - 变化：ProcessMemoryManager 成为地址空间唯一权威（699e042）——CLONE_VM 共享m（线程），fork 深拷贝 mm（进程），exec 原子替换 mm，VMA元数据、munmap/mremap、共享页和退出释放形成完整闭环 之前说过了
  - LA PTE 位扩展：新增 PTE_COW（写时复制软件位）、PTE_NX/NR（不可执行/不可读）
  - LA 四窗口地址转换：to_phy()/to_vir()/to_io()/to_dma()
  - vmaspace 与 vmobject 抽象
    - VMASpace：进程地址空间的索引视图，取代直接遍历 VMA 链表
    - VMObject：物理页的抽象容器，封装页面所有权和回写逻辑
    - 迁移路径（fb54974 → f8c0de5）：
    - execve 段与用户栈（926f6ce）
    - 缺页与私有映射 COW（c423e41）
    - mmap 与 shm 运行路径（1817288）
    - 收口：统一 mmap/shm/copy_in/copy_out 到新模型（f8c0de5）

  #### 4.2.5 缺页异常处理（COW + 懒分配）
图 4-6 缺页异常处理流程

  - Lazy page allocation：普通匿名私有页改为懒分配（e11078a），不再在 mmap时预分配物理页，首次访问某虚拟地址时才分配物理页，避免预分配浪费
  - COW（Copy-on-write）：fork 时父子共享所有页（只读），任一方写入时触发缺页 → 复制该页 →更新双方页表为可写
  - mmap_handler 在缺页时依次尝试：① COW 解决 ② TLB 残留重试（LA） ③ VMA 范围内的惰性分配
  ④ 栈的 VMA 向下扩展（MAP_GROWSDOWN）
  - 变化：LA 侧 resolve_cow_page() 配合 TLB 精准失效（3c0be3c）
  - MAP_SHARED 对接 ShmManager 的逻辑统一到 ProcessMemoryManager（dee723b）

---

 ## 第五章 进程与线程管理

  > 旧报告对应：旧 §2.4（进程控制）


  ### 5.1 进程控制块（PCB）

  - 组成：global_id / pid / tid / ppid / pgid，全局进程池 k_proc_pool[]
  - 状态机：UNUSED → USED → RUNNABLE ↔ RUNNING → SLEEPING / ZOMBIE  七种状态，比旧报告多了一种 STOPPED
  - 变化：新增 TGID、TLS、CLONE_CHILD_CLEARTID（979ac9b）、nice
  值（87b9eec）

  ### 5.2 进程调度
> 图 5-1 上下文切换

  - 上下文切换：swtch.S 保存/恢复 callee-saved 寄存器，swtch(&old, &new)
  - 轮转调度：Scheduler 遍历 PCB 找 RUNNABLE → swtch 切入 → 时间片到 →
  yield
  - 优先级调度
  - 变化：setpriority/getpriority 同时作用于 CPU nice 和 I/O
  分类（87b9eec）

  ### 5.3 进程生命周期

  - 创建（fork / clone / clone3）：alloc_proc() → 深拷贝或共享 mm（详见
  §4.2.4）
    → 设 PID/TID。clone flags 对齐 Linux，新增 clone3（4fe2380）
  - 装载（execve）：解析 ELF → 加载段 → 构造新地址空间 → 原子替换旧mm（详见 §4.2.4）
    → 设 PC/SP。新增动态 ELF（PT_INTERP）、shebang（a2dc3e0）、LA 16KB对齐（3803a69）
  - 退出（exit / wait）：关闭 fd → 清理信号/futex → 释放 mm → ZOMBIE → 发
  SIGCHLD。
     SIGCHLD 逻辑重构（5f21ad6），LA 关中断保护（0cbf984），errno
  语义对齐（31eea18）
  - 线程：clone(CLONE_VM|CLONE_THREAD)，TGID/TID 区分，TLS → tp。LA pthread——LL/SC、TLB 并发修复（a74491f、3c0be3c、3890255）




## 第六章　文件系统
  > 旧报告对应：旧 §2.5（文件系统架构）+ 旧 §4.2（VirtIO）+ 旧 §4.3（块设备）+ 旧第五章（设备管理）。
  > 叙述逻辑：自底向上——先讲设备抽象，再讲磁盘怎么读写，然后讲内核怎么抽象，最后讲用户看到什么。
  > 用户读写文件 → VFS → ext4 → Buffer → virtio-blk → 磁盘
  > 2026 年设备管理不再独立成章，VirtualDevice 体系作为设备抽象层归入文件系统章，与 virtio-blk 形成「抽象 → 实现」的叙述递进。

### 6.1 设备抽象层

  #### 6.1.1 虚拟设备基类的设计理念

  - VirtualDevice 四个纯虚接口：type() / handle_intr() / read_ready() / write_ready()
  - 为上层 VFS 和 epoll 提供统一的设备就绪判断入口
  - 旧报告单文件实现 → 2026 年按 RISC-V/LA 拆分，接口定义不变

  #### 6.1.2 字符设备的特性与应用

  - CharDevice 继承 VirtualDevice，增加字符级读写接口（get_char / put_char）
  - support_stream() 判断是否支持流式操作
  - 控制台输入输出、串口通信等逐字符交互的场景

  #### 6.1.3 块设备的架构与优化

  - BlockDevice 继承 VirtualDevice，BufferDescriptor 描述缓冲区
  - read_blocks / write_blocks 支持同步/异步、scatter-gather
  - 旧架构专用磁盘驱动 → 统一 virtio-blk 替代（详见 §6.2）
  - BlockDeviceIoctlState：2026 年从 syscall_handler 抽离 [→ 5319d08]

  #### 6.1.4 流设备的高级功能

  - StreamDevice 继承 CharDevice，封装字节流 read/write
  - redirect_stream 支持 I/O 重定向
  - 缓冲区管理和线路状态查询

  #### 6.1.5 Loop 设备

  - /dev/loop0~7，将普通文件映射为块设备
  - ISO 镜像挂载、文件系统测试等场景

### 6.2 块设备驱动与 I/O 调度

  #### 6.2.1 统一 virtio-blk 框架

  - 旧：RISC-V 和 LA 各一套独立驱动（virtio2.hh / virtio_disk.cc）
  - 新：三层统一——Transport（隔离 MMIO/PCI）→ Device（块设备抽象）→Queue（请求排队、descriptor 管理、完成回收）底层是 Transport（屏蔽 MMIO 和 PCI 的硬件差异），中间是Queue（管请求排队和 descriptor），上层是 Device（对上只提供一个submit_and_wait 接口）。ext4 要读磁盘，只管发请求，不关心下面是 RISC-V还是 LA。
  - IoRequest 携带块号、读写方向、进程 PID/nice
  - 验证：双架构 iozone 通过（6bdca8c）

  #### 6.2.2 priority-borrow I/O 调度


  - 新：nice 值映射 service class，每 class 内 per-process flow轮转，高优先先走、低优先借空闲带宽
  - 验证：A/B 吞吐对比实验（4099546）、iozone 四组合得分
  89.703（dde2ae4）

  ### 6.3 系统文件

  - VFS 的角色：对上提供统一的文件操作接口，对下对接 ext4/FAT32/虚拟文件
  - VFS 架构：文件打开流程（路径解析 → 找到 inode → 创建 file 对象 → 返回
  fd）
  - 七种文件派生类型：normal_file、device_file、pipe_file、socket_file、dir_file、virtual_file、epoll_file（2026 新增，de2f2c9）

> 图 6-1 虚拟文件系统架构

  ### 6.4 VFS 核心元数据

  #### 6.4.1 Buffer 层
 >图 6-2 Buffer 缓冲区管理
 读磁盘有缓冲的优化，问前面几次提交关于让iozone能够运行，做了什么优化。写一个小节，就是你说了ext4 cache 优化，批量读写路径提
速（dde2ae4）优化了批量读写，相邻块合并提交，减少和磁盘的来回次数

  - Buffer/Block 缓存，LRU 淘汰
  - 变化：ext4 cache 优化，批量读写路径提速（dde2ae4）优化了批量读写，相邻块合并提交，减少和磁盘的来回次数

  #### 6.4.2 SuperBlock 和 Inode
    记录磁盘是什么文件系统
  - 通用 superblock 结构 + ext4 专用 ext4_sblock
  - super_operations 函数表、dirty inode 链表

  - VFS inode + inode_operations 函数表 + ext4特有信息（vfs_ext4_inode_info）  记录文件大小、权限、数据块在哪
  - 变化：操作表扩展，补充 splice、xattr 相关回调（1b6de72）

  ### 6.5 ext4 与 FAT32

  - ext4：主力根文件系统，SuperBlock → Inode → 数据块，I/O 走统一virtio-blk 队列
  - FAT32：数据盘（/data）或回退挂载，纠正旧报告「对等根文件系统」的表述
  - 变化：两套文件系统不再直调架构专用驱动，统一走 virtio-blk（6bdca8c）

  ### 6.6 文件操作

   其他操作   一点点变化，提一下就好
  - fcntl：F_DUPFD / CLOEXEC / O_NONBLOCK /文件锁（F_SETLK/SETLKW/GETLK）/ F_SETPIPE_SZ / F_ADD_SEALS，  
> 图 6-3 文件控制调用
  - ioctl：2026年按设备 类型 拆分——BlockDeviceIoctlState、SocketIoctlCompat（5319d08）
  - xattr：setxattr / getxattr / listxattr 及 inodeflags （FS_IOC_GETFLAGS/SETFLAGS）在文件上挂键值对
  - 变化：splice 数据搬运（1b6de72）、fanotify监控文件访问（4f5ac77）、memfd sealing修复（f8ea061）

  ### 6.7 挂载与路径解析
    多个磁盘 -> 目录树
  - 旧报告未深入，2026 年独立成节
  - mount / umount / bind mount 完整实现（3803a69）
  - mount namespace 视图隔离,让不同进程看到不同的挂载视图
  - 符号链接解析：递归 → 有深度上限的迭代（47829cc）
  - mount 路径处理优化（2e6a369）

  ### 6.8 虚拟文件与特殊设备

  - /proc/：meminfo、cpuinfo、version、mounts、self/maps、self/stat、uptime
  等
  - /proc/sys/：pid_max、shmmax、shmmni 等
  - /etc/：passwd、ld.so.preload
  - /dev/：null、zero、loop-control、loop0~7
  - loop 设备：ISO 镜像挂载、I/O 转发
---

## 第七章　进程间通信
  > 旧报告对应位置：旧 §2.6.3（内存共享）、旧 §2.6.4（memfd）、旧§2.6.5（管道）。
  > 旧 §2.6.1（信号）和旧 §2.6.2（futex）
  > mmap/MAP_SHARED 的缺页与 VMA 细节移入第四章 §4.2.5。
  > 管道作为 VFS file 派生类的注册、open/close 流程在第六章 §6.3。
  > 本章聚焦 IPC 机制的 API 语义、内部数据结构与跨进程协作流程。
  > **2026 年新增：epoll 事件通知、eventfd 事件计数。**

  进程之间怎么传数据、怎么知道数据来了

  ### 7.1 信号（含 POSIX Timer）

  - 数据结构：signal_frame + sigaction + pending 信号集
  - 投递：usertrap 返回前 handle_signal() → 按 sigaction 分发 → sigreturn
  恢复
  - 默认处理：terminate / coredump / ignore
  - POSIX Timer：timer_create/settime/gettime/delete + ITIMER →到期发信号（1b894aa、b8e52a7）
  - 变化：SIGCHLD 彻底重构（5f21ad6）、libctest 信号路径优化（5f21ad6）

  ### 7.2 futex
> 图 7-1 futex 工作机制
  - wait：值不匹配 → EAGAIN，匹配 → SLEEPING
  - wakeup：取 n 个等待者 → RUNNABLE
  - Robust list：线程退出自动遍历解锁，配合 CLONE_CHILD_CLEARTID 每个线程把自己正在等的 futex 登记在链表中，知道前主人死了，自己接管。
  - 变化：LA pthread futex 并发修复（0cbf984、979ac9b），FUTEX_REQUEUE支持

  ### 7.3　管道
  > 图 7-2 管道基本实现
  > 图 7-3 管道管理器

  内核中转
  - pipe/pipe2 创建、fork 后 fd 继承
  - 循环缓冲区：默认 4KB，阻塞/非阻塞语义，SIGPIPE
  - fcntl F_SETPIPE_SZ / F_GETPIPE_SZ
  - FifoManager：路径→Pipe 映射，open 时 rendezvous
  - 变化：O_ASYNC 标志位修正 [→ bfa0e73]、pipe sleep bug [→ c40a7aa]

  ### 7.4　共享内存
  > 图 7-4 共享内存
  绕过内核直接访问
  
  SysV 共享内存：通过 key 找段
  - shm_segment
  结构（key/size/权限/附加列表/auto_destroy_on_last_detach）
  - ShmManager：create/attach/detach/delete，空闲块首次适配+合并
  - shmget/shmat/shmdt/shmctl API，ftok 转换
  - fork 时附加记录复制，退出时批量清理
  - 与 mmap MAP_SHARED 的协作（详见第四章 §4.2.5）
  - [→ dee723b, c9ac7c4]

  memfd：通过 fd传递
  - memfd_create 创建匿名 inode，惰性分配物理页
  - F_SEAL_WRITE/SHRINK/GROW/SEAL 密封语义
  - 通过 fd 传递实现跨进程共享
  - [→ f8ea061]

  ### 7.5　就绪通知

  epoll：
  - epoll_file 结构，eastl::vector<epoll_watch_entry> 关注列表
  - epoll_create1 / epoll_ctl / epoll_pwait
  - 统一就绪接口：read_ready()/write_ready()
  - LT/ET 模式、EPOLLONESHOT
  - [→ de2f2c9]

  eventfd： 一个计数器，写端加、读端清零，轻量通知
  - 64 位计数器，write 累加、read 清零
  - 与 epoll 的就绪判断集成
  - [→ 5319d08]
---

## 第八章　系统调用

> 旧报告对应位置：旧第三章（系统调用框架 + 实现 + 兼容性）。
> 最大变化：巨型 syscall_handler 拆分为多领域模块；新增 capability 和 timex；绑定数 224 → 243。


  ### 8.1　系统调用概述
 > 图 8-1 系统调用示意图

  - 用户态→内核态的唯一入口：ecall 触发 trap，usertrap() 识别syscall，返回走 usertrapret()
  - 调用号通过 a7 传递，返回值写入 trapframe 的 a0
  - 旧报告 224 个绑定 → 2026 年 243 个，表项默认 ENOSYS 再显式覆盖
  - 新增和补齐的 syscall 按附录 A.6 列出

  ### 8.2　系统调用流程

  执行路径：
  - ecall → stvec 跳转 uservec.S → 保存寄存器、切换内核页表 → usertrap()
  - usertrap() 读 ecode → 确认 syscall → invoke_syscaller()
  - invoke_syscaller() 从 trapframe 取 a7 → 查表 → 调用处理函数 →
  返回值写入 a0
  - usertrapret() → 恢复寄存器 → sret/ertn 回用户态

  参数获取：
  - 六个参数从 a0–a5 取，超出部分按架构 ABI 走寄存器或栈
  - copy_in/copy_out：用户态指针经页表校验和权限检查后才可读写
  - 参数结构体（stat、timespec 等）跨 RISC-V/LA 的对齐处理 [→ 5319d08]

  系统调用表：
  - syscall_defs.hh 定义 SYS_xxx 常量，syscall_handler.hh 声明处理函数
  - BIND_SYSCALL 宏注册调用号与函数指针，表大小 512 项，未绑定返回 ENOSYS

  分发器：
  - invoke_syscaller 查表取函数指针，调用后收集负 errno
  - Linux errno 返回语义修正 [→ 31eea18]

  ### 8.3　系统调用实现

  - 旧报告基于单一巨型 syscall_handler.cc
  - 2026 年拆分为三分模块 [→ 5319d08]：
    - syscall_abi：参数层，隔离 RISC-V/LA 的 ABI 差异
    - sysio：I/O 类（文件、socket、设备 ioctl）
    - sysproc：进程类（fork/clone/exec/exit/wait/signal）
  - 领域管理类：FileDescriptorAccess（统一 fd
  查找）、SocketIoctlCompat（socket ioctl
  兼容）、BlockDeviceIoctlState（块设备 ioctl）
  - 独立抽出的子系统：CapabilityManager（capget/capset）、TimexController
  （adjtimex/clock_adjtime）、ConsoleTermios（tty/termios 状态）
---


## 第九章　网络系统模块
> 旧报告对应位置：旧第四章（网络系统模块）。
> 最大变化：从「ONPS 框架 + BSD Socket 占位」→「真实 loopback TCP/UDP数据面」；iperf/netperf 验证通过。
> onps少讲，重点放在virtio <-> onps 兼容层设计和 socket <-> onps 兼容层设计上

### 9.1　网络系统架构概述
> 图 9-1 网络模块架构示意图
  - 整体分层：Socket 接口 → 协议栈（TCP/UDP → IP → Ethernet）→ VirtIO-Net
  驱动
  - 2026 年现状：loopback 路径完全可用（真实 payload传递），ONPS/VirtIO-Net 框架保留用于后续外网接入
  - 初始化流程：VirtIO-Net 设备发现（RISC-V MMIO / LA PCI）→ MAC 读取 →ONPS 协议栈注册 → loopback 端口表初始化
  - 「loopback 已验证 + ONPS框架待验证」
  - [→ 783e881]

  ### 9.2　VirtIO 网络适配器

  - VirtIO-Net 驱动：MAC 地址读写、virtqueue 管理
  - 可访问 host 服务，QEMU 网关 ICMP 已跑通 [→ fac082d]
  - 与统一 virtio-blk 共用 transport 层（第六章 §6.2.1），但网络数据路径独立


  ### 9.3　核心网络协议栈

  - ONPS 框架保留：Ethernet → IP（含 ICMP/ping）→ TCP/UDP 状态机
  - 2026 年重点不在扩展协议栈，而在让 loopback 路径跑通完整的 TCP/UDP
  语义
  - loopback 不走 Ethernet/IP 层，直接在内存中成对连接 socket，通过
  sleep/wakeup 传递 payload
  - TCP：bind/listen/connect/accept、backlog、双向 stream、close/shutdown
  唤醒
  - UDP：bind、datagram queue、sendto/recvfrom、源地址回填
  - 阻塞/非阻塞、队列背压、信号中断（EINTR）、IPv6 loopback 兼容
  - MSG_MORE 延迟发送、sendmmsg/recvmmsg 批量消息 [→ 7c6cbe6]
  - [→ 783e881, eb73966]

  ### 9.4　BSD Socket 接口

  Socket 文件抽象：
  - socket_file 继承 file，对接 VFS 的 read/write/poll 接口
  - 全局 loopback 端口表管理 listener 和 UDP binding

  连接管理：
  - TCP：socket() → bind() → listen() → accept() 返回新 fd → connect()
  创建成对 socket_file
  - UDP：socket() → bind() → sendto()/recvfrom()，每次发送生成独立atagram

  数据传输：
  - TCP 流式：send/recv，内核缓冲区中转，阻塞直到对端消费或窗口可用
  - UDP 数据报：sendto/recvfrom，保持消息边界

  就绪通知：
  - O_NONBLOCK → EAGAIN，信号 → EINTR
  - 与 epoll（第七章 §7.3）集成：套接字的 read_ready/write_ready 判断
  - poll/select 兼容

  系统调用：
  - socket/bind/listen/connect/accept/send/recv/sendto/recvfrom
  - getsockopt/setsockopt（SO_REUSEADDR 等）
  - SocketIoctlCompat：socket ioctl 的兼容视图 [→ 5319d08]
  - netperf 连续运行稳定性：修复 server 启动竞态和信号中断 [→ 9bccd94]
  - [→ 62eff70, eb73966]



## 第十一章 logging系统


## 第十一章 总结与展望
 ### 11.1工作总结
  1. 完成 RISC-V 与 LoongArch 双架构支持，包括 DTB动态内存发现、APIC+ExtIOI中断适配、TLB 精准失效、LL/SC 原子指令修复、统一 virtio-blk跨架构框架。

  2. 通过面向对象设计完成进程与内存管理模块，ProcessMemoryManager统一地址空间所有权，fork/exec/COW/懒分配路径跨架构一致，支持 clone3、线程调度与POSIX Timer。

  3. 实现独立 VFS 层，七种 file 派生类型，ext4 主根文件系统，FAT32
  数据盘，mount/bind mount，/proc、/dev虚拟文件，fcntl/ioctl/splice/xattr/fanotify 补齐。

  4. 实现真实 loopback TCP/UDP 数据面，TCP 全流程、UDP 数据报、MSG_MOREsendmmsg/recvmmsg，iperf/netperf 通过，ONPS/VirtIO-Net 框架保留。

  5. 系统调用 224→243，巨型 handler 拆为 syscall_abi/sysio/sysproc，Capability、Timex、Termios 独立抽离，errno 语义对齐 Linux。

  6. 完成 musl/glibc 双 libc 和 BusyBox ash 适配，动态 ELF（PT_INTERP）与shebang 执行，评测/Shell 双模式构建。

  7. 实现完整 IPC：信号（SIGCHLD 重构）、Futex（robustlist）、管道/FIFO、SysV 共享内存、memfd 密封、epoll（LT/ET）、eventfd。

  8. 建立四组合评测体系（RISC-V/LA × musl/glibc），LTP 流水线，iozone89.703、libcbench 129.973，priority-borrow I/O调度实验，构建与日志系统规范化。

  9. 工程规范：C++23 freestanding、禁用异常/RTTI，删除递归符号链接、旧架构驱动；I/O 调度方案经历mClock→priority-borrow→mClock 重新移植迭代，目录结构与复现命令收归标准路径。

 ### 11.2经验总结
 ### 11.3未来计划


## 附录 A　重要类的具体字段解析

本附录列出系统中关键数据结构的核心字段与方法，供阅读正文时快速查阅。

### A.1　Pcb（进程控制块）

| 字段 | 说明 |
|---|---|
| **标识与状态** | |
| pid, tid, tgid, ppid, pgid | 进程号、线程号、线程组号、父进程号、进程组号 |
| state | UNUSED→USED→RUNNABLE↔RUNNING→SLEEPING/ZOMBIE/STOPPED |
| exit_code | 退出码，父进程通过 wait 获取 |
| **地址空间与文件** | |
| mm | ProcessMemoryManager*，CLONE_VM 共享，fork 深拷贝 |
| files[] | fd 表，每项为 file* |
| **信号与同步** | |
| sighand[] | sigaction 表，exec 时重置为默认 |
| pending_signals | sigset_t，待处理信号集 |
| signal_frame* | 信号栈帧链表头 |
| **线程相关** | |
| tls | 线程局部存储基址，写入 tp 寄存器 |
| clear_child_tid | 退出时清零并 futex wake 父进程 |
| robust_list_head | futex robust list 头，退出时遍历解锁 |
| **调度与 I/O** | |
| nice | 静态优先级，CPU 调度 + I/O 分类共用 |

### A.2　ProcessMemoryManager（进程内存管理器）

| 字段 / 方法 | 说明 |
|---|---|
| **地址空间结构** | |
| page_table | 进程页表根 |
| vma_list | VMA 链表：ELF 段、heap、mmap、stack |
| heap_start, heap_end | brk 堆起止，grow_heap / shrink_heap |
| mmap_cursor | mmap 自动选地址的搜索起点 |
| ref_count | 引用计数，>1 触发 COW 深拷贝 |
| **进程操作** | |
| load_elf() | 解析 ELF，按 p_vaddr 和权限装载各段 |
| fork_copy() | fork 时深拷贝页表和 VMA 链 |
| exec_replace() | exec 成功后原子替换地址空间 |
| release() | 退出时回收物理页、清空页表 |

### A.3　信号相关结构

| 字段 / 方法 | 说明 |
|---|---|
| **信号集与处理动作** | |
| sigset_t | uint64 sig[1]，每个 bit 对应一个信号 |
| sigaction | sa_handler + sa_flags + sa_mask |
| kernel_sigaction_abi | syscall ABI 布局：handler→flags→restorer→mask |
| **运行时结构** | |
| signal_frame | mask + tf + next，链表串联嵌套信号 |
| usercontext | flags + link + stack + sigmask + mcontext |
| machinecontext | pc + gregs[32] + flags，LA 含 extcontext[] |

### A.4　file 类及其派生类

| 字段 / 方法 | 说明 |
|---|---|
| **基类 file** | |
| _attrs | FileTypes（FT_NORMAL/FT_DIR/FT_DEVICE/FT_PIPE/FT_SOCKET…）+ mode |
| _stat | Kstat：inode 号、大小、设备号、时间戳 |
| _ref_count | 引用计数，dup 加、close 减、归零析构 |
| read_ready() | 读就绪判断，供 epoll/poll 使用 |
| write_ready() | 写就绪判断，供 epoll/poll 使用 |
| **常规文件** | |
| normal_file | 对接 ext4/FAT32 inode |
| dir_file | 目录遍历，read_sub_dir 对接 getdents64 |
| **特殊文件** | |
| device_file | 对接 DeviceManager，按设备号分发 read/write |
| virtual_file | /proc、/etc 等动态生成内容 |
| **管道** | |
| pipe_file::_pipe | 指向 proc::ipc::Pipe |
| pipe_file::_can_read, _can_write | 端点方向 |
| pipe_file::_fifo_path | FIFO 路径 |
| **网络** | |
| socket_file::state | TCP 状态（CLOSED/LISTEN/ESTABLISHED 等） |
| socket_file::listener, peer | 监听端与对端 socket 互指 |
| socket_file::recv_queue, send_queue | 内核收发缓冲区 |
| **事件通知** | |
| epoll_file::_watch_list | vector<epoll_watch_entry>，关注 fd 列表 |

### A.5　VFS 核心元数据

| 字段 / 方法 | 说明 |
|---|---|
| **超块** | |
| SuperBlock::device | 所在块设备 |
| SuperBlock::blocks_count, inodes_count | 磁盘布局 |
| SuperBlock::super_operations | read_inode、write_inode、alloc_block |
| **索引节点** | |
| Inode::inode_num, size, mode, blocks[] | 文件元数据与数据块索引 |
| Inode::inode_operations | readpage、write_begin、getattr |
| **块缓存** | |
| Buffer::block_no | 块号 |
| Buffer::data | void*，块数据指针 |
| Buffer::is_dirty, ref_count | 脏标志与引用计数，LRU 管理 |

### A.6　共享内存

| 字段 / 方法 | 说明 |
|---|---|
| **段结构** | |
| shm_segment::shmid, key | 段标识与 System V 查找键 |
| shm_segment::size, real_size | 用户大小 / 页对齐实际大小 |
| shm_segment::shmflg | 9 位权限（ugo rwx） |
| shm_segment::attached_addrs | vector<(tid, addr)>，按线程记录附加地址 |
| shm_segment::phy_addrs | 物理页基址 |
| shm_segment::auto_destroy_on_last_detach | mmap 后端自毁标志 |
| **管理器** | |
| ShmManager::segments | unordered_map<shmid, segment> |
| ShmManager::free_blocks | vector<free_block>，按地址排序，首次适配+合并 |

### A.7　管道

| 字段 / 方法 | 说明 |
|---|---|
| **数据通路** | |
| Pipe::_buffer[] | 动态分配的循环缓冲区 |
| Pipe::_head, _tail, _count | 读位置、写位置、当前字节数 |
| Pipe::_pipe_size | 默认 4KB，可调至 256B–64KB |
| **端点状态** | |
| Pipe::_read_is_open, _write_is_open | 关闭后 read 返 0 / write 发 SIGPIPE |
| Pipe::_nonblock | 非阻塞标志 |
| Pipe::pipe_flags | O_ASYNC 等标志位 |
| **等待队列** | |
| Pipe::_read_waiter, _write_waiter | 阻塞等待的 PCB，数据到达/空间释放时唤醒 |
| **FIFO 管理** | |
| FifoManager::_fifo_map | unordered_map<路径, FifoInfo{pipe, reader_count, writer_count}> |

### A.8　设备管理

| 字段 / 方法 | 说明 |
|---|---|
| **抽象基类** | |
| VirtualDevice::type() | dev_block / dev_char |
| VirtualDevice::handle_intr() | 中断入口 |
| VirtualDevice::read_ready() | 读就绪查询 |
| VirtualDevice::write_ready() | 写就绪查询 |
| **块设备** | |
| BlockDevice::BufferDescriptor | {buf_addr, buf_size} |
| BlockDevice::read_blocks() | 异步读，支持 scatter-gather |
| BlockDevice::write_blocks() | 异步写，支持 scatter-gather |
| **字符设备** | |
| CharDevice::get_char() | 同步读一个字符 |
| CharDevice::put_char() | 同步写一个字符 |
| StreamDevice::_stream | 下层 CharDevice 指针 |
| StreamDevice::redirect_stream() | I/O 重定向 |
| StreamDevice::read() | 字节流读 |
| StreamDevice::write() | 字节流写 |
| **注册** | |
| DeviceTableEntry | {VirtualDevice*, device_name} |
| DeviceManager::_device_table[] | 固定数组，前三保留位 stdin/stdout/stderr |


## 附录 B　系统调用实现列表

 本报告覆盖时，F7LY-OS 已绑定 243 个系统调用，按功能分为十组。

  ### B.1　进程管理

  | 函数 | 说明 |
  |---|---|
  | sys_fork() | 深拷贝地址空间，创建子进程 |
  | sys_vfork() | 创建子进程，父进程阻塞至子进程 exit 或 exec |
  | sys_clone() | 按 clone flags 创建线程或进程 |
  | sys_clone3() | clone 扩展版本，支持更多 flags |
  | sys_execve() | 解析 ELF 或 shebang，装载并执行 |
  | sys_exit() | 当前线程退出 |
  | sys_exit_group() | 线程组全部退出 |
  | sys_wait4() | 等待子进程状态变化 |
  | sys_waitid() | 等待子进程，支持更细粒度选项 |
  | sys_getpid() | 获取进程号 |
  | sys_getppid() | 获取父进程号 |
  | sys_gettid() | 获取线程号 |
  | sys_getuid() | 获取实际用户 ID |
  | sys_geteuid() | 获取有效用户 ID |
  | sys_getgid() | 获取实际组 ID |
  | sys_getegid() | 获取有效组 ID |
  | sys_setuid() | 设置用户 ID |
  | sys_setgid() | 设置组 ID |
  | sys_setreuid() | 设置实际和有效用户 ID |
  | sys_setregid() | 设置实际和有效组 ID |
  | sys_getpgid() | 获取进程组号 |
  | sys_setpgid() | 设置进程组号 |
  | sys_setsid() | 创建新会话 |
  | sys_getpriority() | 获取 nice 值 |
  | sys_setpriority() | 设置 nice 值 |
  | sys_personality() | 设置进程执行域 |
  | sys_set_tid_address() | 设置 clear_child_tid 地址 |
  | sys_set_robust_list() | 设置 futex robust list 头 |
  | sys_get_robust_list() | 获取 futex robust list 头 |
  | sys_sched_yield() | 主动让出 CPU |
  | sys_sched_getaffinity() | 获取 CPU 亲和性 |
  | sys_sched_setaffinity() | 设置 CPU 亲和性 |
  | sys_getrlimit() | 获取资源限制 |
  | sys_setrlimit() | 设置资源限制 |
  | sys_getrusage() | 获取资源使用统计 |
  | sys_prctl() | 进程控制操作 |

  ### B.2　内存管理

  | 函数 | 说明 |
  |---|---|
  | sys_brk() | 调整 heap 末端 |
  | sys_mmap() | 创建文件映射或匿名映射 |
  | sys_munmap() | 解除映射 |
  | sys_mremap() | 调整已有映射的大小或位置 |
  | sys_mprotect() | 修改映射页权限 |
  | sys_madvise() | 通知内核内存使用意图 |
  | sys_msync() | 同步 mmap 脏页到文件 |
  | sys_shmget() | 创建或查找共享内存段 |
  | sys_shmat() | 附加共享内存段到地址空间 |
  | sys_shmdt() | 解除共享内存段附加 |
  | sys_shmctl() | 控制共享内存段 |
  | sys_memfd_create() | 创建匿名内存文件 |
  | sys_mseal() | 对内存映射施加 seal |

  ### B.3　文件系统

  | 函数 | 说明 |
  |---|---|
  | sys_openat() | 打开或创建文件 |
  | sys_openat2() | openat 扩展版本 |
  | sys_close() | 关闭文件描述符 |
  | sys_read() | 读文件 |
  | sys_write() | 写文件 |
  | sys_readv() | 矢量读 |
  | sys_writev() | 矢量写 |
  | sys_pread64() | 指定偏移读 |
  | sys_pwrite64() | 指定偏移写 |
  | sys_lseek() | 移动文件偏移 |
  | sys_fstat() | 通过 fd 获取文件元数据 |
  | sys_fstatat() | 通过目录 fd + 路径获取元数据 |
  | sys_statx() | 扩展元数据查询 |
  | sys_truncate() | 按路径截断文件 |
  | sys_ftruncate() | 按 fd 截断文件 |
  | sys_link() | 创建硬链接 |
  | sys_unlink() | 删除硬链接 |
  | sys_symlink() | 创建符号链接 |
  | sys_readlink() | 读取符号链接目标 |
  | sys_mkdirat() | 创建目录 |
  | sys_renameat() | 重命名文件或目录 |
  | sys_renameat2() | 重命名扩展版本 |
  | sys_getdents64() | 读取目录条目 |
  | sys_getcwd() | 获取工作目录 |
  | sys_chdir() | 切换工作目录 |
  | sys_fchdir() | 通过 fd 切换工作目录 |
  | sys_mount() | 挂载文件系统 |
  | sys_umount2() | 卸载文件系统 |
  | sys_statfs() | 获取文件系统统计 |
  | sys_fstatfs() | 通过 fd 获取文件系统统计 |
  | sys_sync() | 全部缓存同步到磁盘 |
  | sys_fsync() | 单文件缓存同步 |
  | sys_fdatasync() | 单文件数据同步 |
  | sys_fcntl() | 文件锁、CLOEXEC、管道容量等 |
  | sys_ioctl() | 设备控制 |
  | sys_dup() | 复制 fd |
  | sys_dup3() | 复制 fd 到指定编号 |
  | sys_pipe2() | 创建管道 |
  | sys_splice() | 两 fd 间搬运数据 |
  | sys_sendfile() | 文件到 socket 直接发送 |
  | sys_flock() | 文件锁 |
  | sys_fchmod() | 修改文件权限 |
  | sys_fchownat() | 修改文件所有者 |
  | sys_faccessat() | 检查文件访问权限 |
  | sys_mknodat() | 创建设备文件或 FIFO |
  | sys_readlinkat() | 读取符号链接 |
  | sys_utimensat() | 设置文件时间戳 |
  | sys_fallocate() | 预分配文件空间 |
  | sys_fanotify_init() | 创建文件监控实例 |
  | sys_fanotify_mark() | 添加监控目标 |

  ### B.4　信号

  | 函数 | 说明 |
  |---|---|
  | sys_kill() | 向进程发送信号 |
  | sys_tkill() | 向线程发送信号 |
  | sys_tgkill() | 向线程组指定线程发送信号 |
  | sys_rt_sigaction() | 注册或查询信号处理函数 |
  | sys_rt_sigprocmask() | 阻塞或解除阻塞信号 |
  | sys_rt_sigpending() | 查询阻塞的待处理信号 |
  | sys_rt_sigsuspend() | 原子替换 mask 并等待信号 |
  | sys_rt_sigreturn() | 从信号处理函数返回 |
  | sys_sigaltstack() | 设置备用信号栈 |
  | sys_rt_sigqueueinfo() | 向进程发送带数据的信号 |
  | sys_rt_tgsigqueueinfo() | 向线程发送带数据的信号 |

  ### B.5　时间与定时器

  | 函数 | 说明 |
  |---|---|
  | sys_clock_gettime() | 获取指定时钟的时间 |
  | sys_clock_settime() | 设置指定时钟的时间 |
  | sys_clock_getres() | 获取指定时钟的分辨率 |
  | sys_gettimeofday() | 获取墙上时间 |
  | sys_settimeofday() | 设置墙上时间 |
  | sys_time() | 返回自 Epoch 的秒数 |
  | sys_times() | 返回进程 CPU 时间 |
  | sys_nanosleep() | 睡眠指定纳秒 |
  | sys_clock_nanosleep() | 基于指定时钟的高精度睡眠 |
  | sys_timer_create() | 创建 POSIX 定时器 |
  | sys_timer_settime() | 设置定时器到期时间 |
  | sys_timer_gettime() | 获取定时器剩余时间 |
  | sys_timer_delete() | 删除定时器 |
  | sys_setitimer() | 设置间隔定时器 |
  | sys_adjtimex() | 调整内核时钟参数 |
  | sys_clock_adjtime() | 调整指定时钟参数 |
  | sys_timerfd_create() | 创建 fd 定时器 |
  | sys_timerfd_settime() | 设置 fd 定时器 |
  | sys_timerfd_gettime() | 获取 fd 定时器 |

  ### B.6　Futex

  | 函数 | 说明 |
  |---|---|
  | sys_futex() | 快速用户空间互斥锁：WAIT / WAKE / REQUEUE 等子操作 |

  ### B.7　事件通知

  | 函数 | 说明 |
  |---|---|
  | sys_epoll_create1() | 创建 epoll 实例 |
  | sys_epoll_ctl() | 添加 / 修改 / 删除关注 fd |
  | sys_epoll_pwait() | 等待关注 fd 就绪 |
  | sys_epoll_pwait2() | epoll_pwait 扩展版本 |
  | sys_poll() | 轮询 fd 就绪状态 |
  | sys_ppoll() | poll + 信号掩码 |
  | sys_select() | 通知 fd 就绪状态 |
  | sys_pselect6() | select + 信号掩码 |
  | sys_eventfd2() | 创建事件通知 fd |

  ### B.8　网络

  | 函数 | 说明 |
  |---|---|
  | sys_socket() | 创建 socket |
  | sys_socketpair() | 创建一对已连接 socket |
  | sys_bind() | 绑定本地地址 |
  | sys_listen() | 标记监听状态 |
  | sys_accept() | 取出一个连接 |
  | sys_accept4() | accept 带标志位 |
  | sys_connect() | 发起连接 |
  | sys_send() | TCP 发送 |
  | sys_recv() | TCP 接收 |
  | sys_sendto() | UDP 发送 |
  | sys_recvfrom() | UDP 接收 |
  | sys_sendmsg() | 矢量消息发送 |
  | sys_recvmsg() | 矢量消息接收 |
  | sys_sendmmsg() | 批量发送消息 |
  | sys_recvmmsg() | 批量接收消息 |
  | sys_getsockname() | 获取本地地址 |
  | sys_getpeername() | 获取对端地址 |
  | sys_setsockopt() | 设置 socket 选项 |
  | sys_getsockopt() | 获取 socket 选项 |
  | sys_shutdown() | 关闭读端 / 写端 |

  ### B.9　权限

  | 函数 | 说明 |
  |---|---|
  | sys_capget() | 获取线程能力集 |
  | sys_capset() | 设置线程能力集 |

  ### B.10　系统信息与其他

  | 函数 | 说明 |
  |---|---|
  | sys_uname() | 返回内核信息 |
  | sys_sysinfo() | 返回系统统计 |
  | sys_syslog() | 内核日志操作 |
  | sys_getrandom() | 获取随机数 |
  | sys_getcpu() | 获取 CPU 编号 |
  | sys_reboot() | 重启或关机 |
  | sys_ioprio_get() | 获取 I/O 优先级 |
  | sys_ioprio_set() | 设置 I/O 优先级 |
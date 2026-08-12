= 内存管理

决赛阶段的内存管理的改动，扩展了物理内存管理能力，以支持决赛 BuildStorm 的 8 GiB QEMU 运行环境。同时，把 ELF、heap、mmap、共享内存、缺页和地址空间回收统一到同一个用户内存模型。这样，BuildStorm 中大量线程、动态库和文件映射可以共享清晰的所有权关系，页表修改也能够在多核下保持一致。

== 物理内存发现与布局

=== 从固定上限到 DTB 内存区间

启动阶段从 DTB 或平台内存 backend 获取实际 RAM 区间，排除内核镜像、DTB、initrd、`reserved-memory` 和硬件空洞后，形成 PMM 的 managed page 范围。PMM 不再使用面向 1 GiB 的固定 `PHYSTOP` 或固定页数数组，因此 RISC-V 与 LoongArch 均可在 8 GiB QEMU 配置下建立完整页管理器。

物理页引用计数表的大小按最终 managed pages 计算，Buddy tree 元数据也按实际页数动态预留。启动布局先计算 metadata 所需空间，再从剩余区域切分普通页、内核 heap、共享内存和页表，避免 metadata 与可分配页重叠。

核心布局可以简化为：

```cpp
MemoryLayout layout = discover_from_dtb();
layout.exclude(kernel_image);
layout.exclude(device_tree);
layout.exclude(initrd);
layout.exclude(reserved_memory);
layout.tree_bytes = buddy_tree_bytes(layout.ram_pages);
layout.refcount_bytes = layout.managed_pages * sizeof(uint16);
initialize_buddy(layout.free_ranges, layout.tree_start);
initialize_refcounts(layout.refcount_start, layout.managed_pages);
```

保证 DTB 描述的内存不会被错误当成连续可用空间，也使同一启动逻辑适用于不同 QEMU 内存大小。

=== 引用计数与批量释放

PMM 为每个 managed page 维护引用计数，页表映射、COW、`VmObject` 和 file page cache 分别持有自己的引用。普通释放只减少引用，最后一个引用归零后才把页交还 Buddy。批量释放路径一次性处理连续页，减少锁竞争和重复的页表清零，适合 exec、munmap 和地址空间 teardown。

== 内核地址空间与页表

=== 双架构页表边界

RISC-V 使用 Sv39/Sv48 约定的页表层级和 trampoline 入口，LoongArch 使用自己的 PTE、TLB refill 和高地址内核映射。公共 `VirtualMemoryManager` 只调用创建页表、映射、撤销映射和失效范围等窄接口，不保存架构页表项格式。

内核页表固定映射内核代码、设备和内核 heap；用户页表只包含用户地址空间、trampoline/trapframe 所需的入口。LoongArch 的用户态返回需要先恢复当前线程 trapframe，再切换到用户页表；RISC-V 则通过独立 ASID 和 trampoline 降低 trap 往返的全量刷新。

=== 内核 heap 与共享区域

内核 heap 从 PMM 明确切分的区域分配，EASTL、文件对象和诊断结构的 `new/delete` 统一经过 heap manager。共享内存和 memfd 的页源由 `VmObject` 管理，不能绕过页引用计数直接把物理页交给某个 syscall。这样，内核 heap、共享页和普通用户页的容量边界互不覆盖。

== 用户地址空间所有权

=== ProcessMemoryManager

`ProcessMemoryManager` 是用户地址空间的唯一所有者，统一持有用户页表、ELF 段、heap、mmap VMA、地址空间引用和 TLB 状态。普通 fork 通过 `clone_for_fork()` 复制地址空间元数据并建立 COW，`CLONE_VM` 线程通过 `share_for_thread()` 共享同一个 mm，exec 成功后再用新 mm 替换旧 mm。

这一所有权关系避免了 ELF loader、`brk`、mmap 和共享内存分别维护地址区间。进程退出、线程退出和 exec 回滚都先解除 PCB 对 mm 的引用，再由最后一个引用负责销毁页表、VMA 和页后端。

=== VMASpace 与 VmArea

`VMASpace` 使用链表保存 `VmArea` 区间对象，并使用 Maple Tree 按地址索引，统一提供 `find`、空洞搜索、split、merge 和 fault 查询。`VmArea` 保存起止地址、权限、映射标志、文件偏移、私有映射 overlay 和后端对象引用；匿名映射、文件映射和共享映射都通过同一套区间元数据表达。

匿名私有 VMA 合并后必须重建 Maple Tree 索引，否则区间尾部虽然已经扩展，后续 `mprotect` 或缺页查询仍可能找不到它。`brk` 则保留历史堆高水位，用来区分堆洞重增长和越过堆边界的动态库/文件映射。

=== VmObject 后端

`VmObject` 提供实际页源：匿名对象按需创建零页或私有页，文件对象从文件页缓存取得页面，共享对象维护跨进程页引用。页表层只负责把后端准备好的页面安装到目标地址，VMA 层只描述区间和权限，避免映射策略直接决定物理页生命周期。

== 缺页、惰性分配与 COW

=== 统一缺页入口

匿名私有映射、ELF 段、用户栈和文件映射都采用惰性补页。缺页处理先由 `VMASpace` 找到包含 fault address 的 `VmArea`，检查访问权限，再调用 `VmObject::prepare_page()` 获取页面并由 VMM 安装 PTE：

```cpp
int handle_page_fault(ProcessMemoryManager &mm,
                      uint64 fault_va, Access access) {
  VmArea *area = mm.vm_space.find(fault_va);
  if (area == nullptr || !area->allows(access))
    return -EFAULT;
  Page *page = area->object->prepare_page(area->offset_of(fault_va));
  if (page == nullptr)
    return -ENOMEM;
  return mm.vmm.map_user_page(fault_va, page, area->pte_flags());
}
```

`copy_in/copy_out` 也使用同一入口处理用户缓冲区的惰性缺页，避免 syscall 路径和 trap 路径各自实现一套补页逻辑。内存锁允许同线程重入，防止 copyout 触发缺页时再次获取同一个 mm 锁而死锁。

=== fork COW 与私有 overlay

fork 时父子地址空间共享只读物理页并增加引用计数，写访问触发 COW fault。若页面来自文件对象，私有写入通过 VMA 的 overlay 记录私有页；共享映射则继续由同一个 `VmObject` 提供页面。只有在引用计数和映射属性允许时，缺页路径才可以原地升级页面，否则分配新页并复制内容。

这种判断同时处理普通匿名页、文件私有映射和共享内存，避免把“物理页引用为 1”简单等同于“页面一定可以原地写入”。

== ASID 与跨核地址空间同步


=== 地址空间进入与活跃 CPU 集合

每个用户 mm 拥有自己的 ASID；内核页表使用固定的内核 ASID，用户线程在进入地址空间时登记当前 CPU 到 `tlb_active_cpu_mask`，离开时清除。`CLONE_VM` 线程共享 mm 和 ASID，fork 创建独立 mm，exec 成功后分配新 mm 的 ASID。

=== TLB shootdown

TLB shootdown 是页表修改后的同步回收步骤：它确保所有仍在使用该地址空间的 CPU 都丢弃旧地址转换后，内存管理器才能安全撤销旧 PTE、改变页面权限或释放对应的物理页。

当 mmap、mprotect、COW 或 VMA teardown 修改页表时，内核只向仍在 `tlb_active_cpu_mask` 中的远端 CPU 发送失效请求：

```cpp
void flush_mm_range(ProcessMemoryManager &mm,
                    uint64 start, uint64 size) {
  mm.tlb_flush_lock.acquire();
  uint64 generation = ++mm.tlb_generation;
  uint64 targets = mm.tlb_active_cpu_mask & ~current_cpu_bit();
  flush_local_asid(mm.user_asid, start, size);
  send_tlb_ipi(targets, mm.user_asid, start, size, generation);
  wait_for_acks(mm, targets, generation);
  mm.tlb_flush_lock.release();
}
```

RISC-V 使用 SBI remote fence，LoongArch 使用 IOCSR IPI；两种实现共享同一生命周期规则：目标 CPU 完成本地失效并确认 generation 后，发起 CPU 才能释放旧 PTE、COW 页或 mm。此前 8 vCPU 诊断中远端调用持续增长的问题，源于 ASID 与 mm 生命周期脱节；将 ASID 绑定到 mm，并只同步活跃 CPU 后，RV/LA 的 8 GiB、8 vCPU、20 轮 TLB/VMA 专项保持稳定。

== 地址空间回收与性能路径

=== mm 最后引用

`free_all_memory()` 使用本次原子引用递减的结果唯一决定最后清理权。PCB 在归还前先摘除 mm 指针，创建失败、exec 回滚和线程退出都遵循同一规则，避免非最后线程误删正在 teardown 的 mm，也避免最后持有者无法被识别造成泄漏。

=== 页缓存与批量 teardown

文件页缓存按对象和页偏移复用已读页面，exec 和重复打开动态库时减少重复读盘。地址空间销毁先批量撤销 VMA/PTE，再集中释放页表、页引用和后端对象；匿名私有 `MADV_DONTNEED` 会实际释放驻留页，而不是只修改 VMA 标志。性能计数和页缓存构造保持默认关闭或显式初始化，避免 freestanding 启动顺序改变内核行为。

== 验证结果

- RISC-V/LoongArch 在 8 GiB QEMU 中均能完成动态 PMM 初始化；实测 managed pages 分别达到约 175 万和 194 万页。
- 双架构 mmapstress03/04/05、SysV SHM、COW/TLB、clone/exec 回滚和 24 轮×8 线程 mm 退出竞态专项无 panic、引用漂移或页表错误。
- 8G/8 vCPU 构建过程中，动态 VMA、文件页缓存、批量 teardown 和跨核地址空间同步均沿正式路径运行；CAgent 文件映射与用户程序启动结果保持稳定。

== 本章小结

本阶段将内存管理从固定容量和多套映射入口，推进到“DTB 发现物理内存、ProcessMemoryManager 统一地址空间、VmObject 提供页源、VMM 安装页表、ASID/TLB 维护跨核一致性”的闭环。该模型同时支撑决赛的大内存 Rust 编译、glibc 动态装载、线程共享地址空间和文件映射压力。

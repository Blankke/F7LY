# VMASpace + VmObject + COW 重构完成记录

## 当前结论

截至 `50a2221`，这轮 VMA 架构重构已经把主要运行路径从“程序段数组 + heap 特例 + 静态 VMA 数组 + SHM 旁路”收敛到 `VMASpace + VmObject + fault_page()` 模型。

已经完成的部分：

- `VMASpace` 成为动态 VMA 对象持有者，使用 `eastl::list<VmArea>` 保存区间对象，用 `VmaMapleTree` 做按地址索引。
- `VmObject` 统一承接匿名页、文件页、SysV SHM 页后端，缺页时返回统一的 `VmPageView` 交给页表层安装。
- `ProcessMemoryManager::fault_page()` 成为 trap、`copy_in/copy_out`、懒映射和 COW 的统一入口。
- `execve()` 的主程序段、解释器段和用户栈已经改为懒注册 VMA，不再在 exec 阶段整段预装。
- `fork()` 对已经迁入 `VMASpace` 的私有区域走页级 COW，不再依赖旧的程序段/heap 特判复制。
- `mmap/munmap/mremap/mprotect/msync/madvise/shm*` 的主路径已经能通过统一地址空间视图遍历和处理动态区域。
- 普通匿名私有页已经改为懒分配，写 fault 时再生成 overlay 私有页。
- LoongArch 用户态返回路径补了 FPU 现场保存，避免 ELF 段懒加载后用户态切换污染浮点寄存器状态。

仍然要按“过渡态”理解的部分：

- `ProcessMemoryManager` 里仍保留 `prog_sections[]`、`heap_start/heap_end`、`VMA vma_data`、`vma_index`、`NVMA` 等旧字段，部分兼容路径还依赖它们。
- `VmArea` 当前仍 typedef 为旧名 `vma`，并保留 `vfd/vfile/offset/backing_kind/backing_shmid` 等兼容字段。
- 当前成果是“主路径迁移和稳定性收口完成”，不是“所有旧契约完全删除完成”。

## 提交回放

- `fb54974 feat(mm): 引入 vmaspace 与 vmobject 骨架`
  - 新增 `vm_area.hh`、`vma_space.*`、`vma_maple_tree.*`、`vm_object.*`、`vma_metadata_utils.*`。
  - 把 VMA 索引从旧数组扫描推进到 Maple Tree 风格的动态索引。
  - `VirtualMemoryManager::allocate_vma_page()` 开始优先走 `VmObject::prepare_page()`。

- `926f6ce feat(mm): 将 execve 段与用户栈迁移到 vmaspace`
  - `execve()` 注册主程序 `PT_LOAD`、解释器 `PT_LOAD` 为 `FileVmObject` 懒文件 VMA。
  - 用户栈注册为 grow-down 的匿名 `VmObject`，guard page 语义进入 `VmArea`。
  - `FileVmObject` 补齐 `filesz < memsz` 时的尾页零填充和 BSS 语义。

- `c423e41 feat(mm): 统一缺页与私有映射 cow`
  - 新增 `ProcessMemoryManager::fault_page()`，统一普通缺页、写时复制和栈增长。
  - RISC-V / LoongArch trap 缺页路径改为委托给 `fault_page()`。
  - `copy_in/copy_out/copy_str_in/ensure_user_*_range` 增加显式目标地址空间参数，避免 exec/clone 时误用当前进程旧 mm。
  - `clone_private_vm_space_for_fork()` 将 `VMASpace` 私有区域复制为页级 COW。

- `1817288 feat(mm): 迁移 mmap 与 shm 运行路径到 vmaspace`
  - `mmap()` 直接创建带 `VmObject/page_offset/area_kind/grow_policy` 元数据的动态 VMA。
  - `munmap/mremap/mprotect/msync/madvise/remap_file_pages` 改为基于 `find_vma_covering()`、`for_each_vma()` 和 `for_each_vma_in_range()`。
  - `/proc/self/maps`、`pagemap`、`status` 等用户可见接口改为遍历统一地址空间视图。
  - `shmat` 注册为 `VMASpace + SysvShmVmObject`，`ShmManager` 继续保留 SysV IPC 元数据职责。

- `f8c0de5 feat(mm): 收口 vmaspace 第五次提交`
  - 清理 mmap/shm 的旧旁路和临时调试入口。
  - 让 `mmap/shm/copy_in/copy_out` 等入口统一走新索引/对象模型。
  - 双架构构建通过，并整理当时的验证状态。

- `337f4a7 uservec添加fpu现场保存，适配当前elf段懒加载防止用户态切换污染`
  - LoongArch `uservec` 保存/恢复用户态 FPU 现场。
  - 配合懒 ELF 段，避免用户态上下文切换后浮点状态被内核或其他任务污染。
  - 同步调整部分懒映射、信号和 trapframe 路径。

- `50a2221 普通匿名私有页改为懒分配`
  - 匿名私有映射不再提前分配源页。
  - 写 fault 时创建 `private_page_overlay` 页；读 fault 可共享零填充源页并通过 COW 标记保护。
  - 恢复完整 initcode 入口，移除验证阶段缩小测试集的临时改动。

## 实现方式

### 地址空间视图

- `kernel/proc/vm_area.hh`
  - `VmArea` 是统一虚拟区间对象。
  - 新权威字段是 `object/page_offset/area_kind/grow_policy/guard_pages/private_page_overlay`。
  - 旧字段暂时保留，主要用于兼容还没有完全删除的旧调用面。

- `kernel/proc/vma_space.hh/.cc`
  - `VMASpace` 负责创建、销毁、克隆和索引 VMA。
  - 区间真实对象放在 `eastl::list<vma> areas_` 中，索引由 `VmaMapleTree index_` 维护。
  - 提供 `find_vma_covering()`、`find_first_vma_at_or_after()`、`find_gap()`、`for_each_in_range()` 等查询能力。

- `kernel/proc/vma_maple_tree.hh/.cc`
  - 实现面向非重叠 VMA 的高扇出 B+Tree 风格索引。
  - 支持覆盖查询、lower_bound、前后相邻 VMA 查询、区间冲突检查和 gap 搜索。

### 页后端对象

- `kernel/proc/vm_object.hh/.cc`
  - `AnonVmObject`：服务匿名映射、heap、用户栈等匿名页。
  - `FileVmObject`：服务 ELF 懒段、解释器段和普通文件映射。
  - `SysvShmVmObject`：服务 SysV SHM 附件区域。
  - `VmObject::prepare_page()` 统一返回物理页、写权限、是否需要 COW、是否是私有 overlay。

- 私有页处理：
  - `VmArea::private_page_overlay` 记录已经私有化的页。
  - 私有映射读 fault 可以使用对象源页并打 COW。
  - 私有映射写 fault 创建 overlay 页并更新 VMA 元数据。

- 文件页处理：
  - `FileVmObject::prepare_page()` 先锁内查缓存，锁外读文件，再回锁二次确认写入缓存。
  - 避免持有 `object_lock_` 进入 `read/fstat` 这类可能 sleep 的路径。

### 统一缺页路径

- `ProcessMemoryManager::fault_page()`
  - 先找覆盖地址的 VMA。
  - 处理 grow-down 栈扩展。
  - 写 fault 先尝试 COW 拆页。
  - 最后调用 `VirtualMemoryManager::allocate_vma_page()` 由 `VmObject` 准备页并安装页表。

- trap 路径：
  - RISC-V 和 LoongArch 的用户态 page fault 都只负责解析 fault 类型和 fault 地址，然后进入 `fault_page()`。

- 用户拷贝路径：
  - `copy_in/copy_out/copy_str_in/ensure_user_*_range` 接受目标 `ProcessMemoryManager`。
  - exec 构造新用户栈、clone 写 child tid、signal frame 写用户态数据时不再误用当前进程地址空间。

### mmap/shm 运行路径

- `ProcessManager::mmap()`
  - 直接向 `VMASpace` 创建匿名、私有文件、共享文件映射。
  - 同步填充对象后端、页偏移、文件有效字节、区域类型和增长策略。
  - `MAP_POPULATE` 失败后通过统一 unmap 路径回滚。

- `munmap/mremap/mprotect/msync/madvise`
  - 基于 `ProcessMemoryManager::find_vma_covering()` 和区间遍历处理动态 VMA。
  - split/trim/rollback 使用 `vma_metadata_utils` 维护 overlay 和对象引用，避免对带 EASTL 成员的 VMA 做平凡复制。

- `ShmManager`
  - 继续负责 SysV IPC ID、key、权限、attach/detach 计数和对象生命周期。
  - 页表安装和页源选择转入 `SysvShmVmObject`。

## 验证记录

已经在提交过程中记录并完成的验证：

- `make build PROFILE=riscv-qemu`
- `make build PROFILE=loongarch-qemu`
- 双架构多轮 `timeout 60s make run ... QEMU_MEM=1G` smoke。

关键结果：

- `fb54974` 后修掉 `private_page_overlay` 被旧路径平凡复制/清零引发的崩溃。
- `FileVmObject` 文件读取移出 `object_lock_` 后，消除了两架构都出现过的 `scheduler.cc:149` 断言。
- `926f6ce` 和 `c423e41` 后，双架构 60s smoke 可自然结束，未见新的 exec 懒段、copy_out 目标 mm、fork COW 页故障问题。
- `f8c0de5` 后，临时 `sys_brk` 轨迹和局部 initcode 入口已经清理，双架构构建通过。
- `50a2221` 后，验证阶段缩小的 initcode 入口已经恢复为完整入口。

需要注意：

- 早期 60s smoke 的外层 `timeout=124` 和 `FAIL LTP CASE ...: 0` 主要来自验证窗口和 wrapper 判定口径，不等价于新增内核崩溃。
- 当前文档记录的是历史提交验证；后续若继续改动内存路径，仍需要按双架构 build + 定向 mmap/shm/fork/pthread 回归重新验收。

## 还有可提升的空间

- 删除旧权威字段：
  - 逐步移除 `prog_sections[]`、`VMA vma_data`、`vma_index`、`NVMA` 静态槽位和相关旧扫描逻辑。
  - 把 `VmArea` 从 `using vma = VmArea` 的兼容形态收口成唯一命名。

- 收敛 heap/brk：
  - 当前 `heap_start/heap_end` 仍作为外部状态存在。
  - 后续可以把 heap 完全表达为 `VmAreaKind::Heap` 的 grow-up 区域，由 `VMASpace` 统一检查冲突和扩缩边界。

- 完善 VMA 操作原语：
  - 在 `VMASpace` 内部提供一等的 `split/merge/protect/remap/unmap` 接口。
  - 减少 `proc_manager.cc` 和 `syscall_handler.cc` 直接操作 VMA 细节。

- 改进对象页缓存：
  - `FileVmObject` 当前已有源页缓存，但还没有完整的页回收、水位控制和脏页生命周期。
  - 后续可以接入 buffer cache/page cache 的统一淘汰策略，避免大文件映射长期占页。

- 强化共享映射语义：
  - `MAP_SHARED` 文件映射写回、`msync`、`ftruncate` 后 SIGBUS、seal 检查等语义还值得继续对齐 Linux。
  - SysV SHM 的 IPC 元数据与页后端已经分层，但 detach、最后引用销毁和命名空间边界还可以继续收敛。

- 降低性能回退：
  - `mprotect` split/rollback、`copy_out` fault、pthread 线程栈 guard page、fork COW 引用维护仍是热点。
  - 后续应继续用 `b_pthread_create_serial1` 和 iozone 做守门项，确保结构收口不换来固定成本回退。

- 扩大验证覆盖：
  - 定向跑 `mmap3`、`mmapstress03`、`mmap-corruption01`、`shm*`、`libcbench pthread`、`iozone`。
  - 长回归汇报仍按“功能点是否修好、当前集合是否无 TFAIL/TBROK/TCONF、整轮是否自然结束”拆开。

## 状态

已完成，待验收：VMA 主路径已经迁移到 `VMASpace + VmObject + fault_page()`；旧静态 VMA/程序段/heap 兼容字段仍需后续继续删除。

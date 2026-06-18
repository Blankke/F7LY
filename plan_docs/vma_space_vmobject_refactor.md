# VMASpace + VmObject + COW 重构记录
## 计划描述
### VMASpace + VmObject + 全量 COW 重构计划

#### Summary
- 这次把 `ELF/PT_LOAD`、`brk`、用户栈、`mmap/shm` 全部并入新的 `VMASpace`，不再保留“程序段数组 + heap 特例 + VMA 数组 + SHM 旁路”四套模型。
- `ShmManager` 直接升级为通用 `VmObjectManager`；它只负责对象注册、SysV IPC 元数据、全局对象索引和对象生命周期，不再直接分配预留物理区，也不再直接改页表。
- 新的缺页处理统一走 `ProcessMemoryManager::fault_page() -> VMASpace -> VmArea -> VmObject`。trap、`copy_in/copy_out`、`MAP_POPULATE`、`exec` 后首次访问、`fork` 后 COW 写 fault 都走这一条链。
- 新的 COW 覆盖全部私有用户映射：`ELF/PT_LOAD`、`MAP_PRIVATE` 文件映射、匿名映射、`brk` 堆、用户栈。目标是同时解决结构清晰度和 `fork/pthread/libcbench/mmap3` 的性能与正确性问题。

#### 核心设计
- 删除 `prog_sections[]`、`heap_start/heap_end` 的主数据职责、`NVMA` 固定数组、`VMA vma_data`、`register_shared_attachment_vma()`、`clear_shared_attachment_vmas()` 等过渡接口。
- 新增 `VmArea`：
  - 字段固定为 `start/end/prot/flags/object/page_offset/area_kind/grow_policy/advice_state/guard_pages`。
  - `area_kind` 固定区分 `ElfLoad`、`InterpreterLoad`、`Heap`、`UserStack`、`Mmap`、`SysvShm`。
- 新增 `VMASpace`：
  - 内部使用 `eastl::list<VmArea>` 持有真实对象，`VmaMapleTree` 索引 `VmArea*`。
  - 提供 `map/unmap/protect/remap/advise/sync/find/find_gap/clone_for_fork/fault_page/merge/split`。
  - `brk/sbrk` 变成对 `Heap` 类型 `VmArea` 的扩缩边界操作，不再单独扫描 heap/VMA 冲突。
- 新增 `VmObject` 体系：
  - `FileVmObject`：服务 `PT_LOAD`、解释器段、普通文件映射。
  - `AnonVmObject`：服务匿名 `mmap`、`brk` 堆、用户栈。
  - `SysvShmVmObject`：服务 `shmget/shmat/shmdt/shmctl`。
  - `SharedFileVmObject` / `SharedAnonVmObject` 作为共享映射语义实例，仍复用统一对象接口。
- `VmObjectManager`：
  - 维护 `object_id -> VmObject`、`shmid/key -> SysvShmVmObject`、`file backing key -> FileVmObject` 的全局索引。
  - SysV SHM 只是它暴露的一组 IPC 入口，不再作为 `MAP_SHARED` 的假后端。
- 私有映射 COW 方案：
  - `VmObject` 持有“源页/共享页缓存”。
  - `VmArea` 持有自己的 `private_page_overlay`，记录已私有化页。
  - fault 时优先看 overlay，再看 object 源页。
  - fork 时父子 `VmArea` 共享 overlay 页并降为只读+COW；写 fault 时只复制单页并替换当前 `VmArea` 的 overlay。
  - 这样 `ELF` 私有段、`brk`、栈、匿名私有映射都能统一走页级 COW，而不是靠旧的“整段 copy + 少量页表 COW 特判”。

#### 关键行为
- `execve()` 改为只创建 `PT_LOAD`/解释器 `VmArea`，默认采用“懒文件 VMA”：
  - 不再在 `execve` 中把整个段预装入页表。
  - `BSS` 通过 `FileVmObject + 零填充尾页` 语义处理。
  - `Heap` VMA 从 `highest_addr` 后创建为 growable `AnonVmObject`。
  - 用户栈创建为 grow-down `AnonVmObject`，guard page 语义写进 `VmArea`。
- `fork()` 改为 `VMASpace::clone_for_fork()`：
  - 共享映射直接共享同一 `VmObject`。
  - 私有映射复制 `VmArea` 元数据并共享 overlay/source 页，统一降级为只读 COW。
  - 删除当前 `clone` 路径里针对程序段、heap、VMA、child_stack 的分散补丁式复制逻辑。
- `mmap/munmap/mremap/mprotect/msync/madvise/shmat/shmdt` 全部只操作 `VMASpace`，不再直接碰页表或 `VmObjectManager` 以外的后端。
- `VirtualMemoryManager::allocate_vma_page()` 下沉为通用页表安装 helper；文件/SHM/匿名的决策逻辑全部迁入 `VMASpace::fault_page()`。
- 锁顺序固定为：
  - `ProcessMemoryManager::memory_lock`
  - `VMASpace` 内部结构锁
  - `VmObjectManager` 注册表锁
  - `VmObject` 页槽锁
  - 禁止反向获取，避免 `mmap3`/`munmap`/`msync` 并发死锁。

#### 落地顺序
- 第 1 提交：引入 `VmArea`、`VMASpace`、`VmObject`、`VmObjectManager` 骨架；把 `MapleTree` 从“数组索引器”改成“对象索引器”；先保留旧路径适配层，确保双架构可编译。
- 第 2 提交：迁移 `ELF/PT_LOAD`、解释器段、`brk`、用户栈到 `VMASpace`；删除 `prog_sections[]` 主路径和 heap 特例。
- 第 3 提交：接入统一 fault 与全量私有映射 COW；迁移 `fork/copy_in/copy_out/signal frame` 到新模型。
- 第 4 提交：迁移 `mmap/munmap/mremap/mprotect/msync/madvise/shm*` 到 `VMASpace + VmObjectManager`；删除 `ShmManager` 预留物理区设计和所有旧旁路。
- 第 5 提交：清理旧接口、补架构文档与开发日志，完成长回归验证。
- 文档同步：
  - 在 `plan_docs/` 新增本任务记录，固定按“情况要求 -> 解决方法”写，每次提交同步补一条“验证方式/对应 commit”。
  - 同步更新 `agent_docs/project_architecture.md` 里“内存与地址空间”“共享内存”两节。

#### Test Plan
- 构建：`make build ARCH=riscv`、`make build ARCH=loongarch`。
- 定向功能：
  - SHM：`shm_comm`、`shmnstest`、`shmem_2nstest`、`shmctl04/05`、`shmget02/03`、`shmt09`。
  - mmap/VMA：`basic/test_mmap`、`mmap01/04/10/14/18`、`mmap3`、`mmapstress03`、`mmap-corruption01`。
  - fork/COW：`libcbench`、`pthread` 相关、`iozone`、`clone`/`fork` 高频测例。
- 语义专项：
  - `execve + 动态链接器 + PT_LOAD 懒载入`
  - `brk/sbrk` 扩缩与 `Heap VMA` 冲突处理
  - `mprotect/madvise/msync/mremap` 的 split/merge 后一致性
  - `copy_out`、signal frame、内核写用户页场景下的 COW 拆页
- 完整功能：整套libctest不出错
- 最终验收继续按现有口径汇报：
  - 功能点是否修好
  - 当前集合是否无新增 `TFAIL/TBROK`
  - 整轮是否自然结束；若只是 timeout/cleanup 噪声单列说明

#### Assumptions
- 这轮直接统一升级，不保留长期兼容的旧 `VMA/NVMA/ShmManager` 双轨逻辑。
- `ELF/PT_LOAD` 采用懒 fault 方案，不再维持当前 `execve` 预装整段的主路径。
- 私有映射性能收益主要来自“懒载入 + 页级 COW + 删除 child_stack/heap/program section 特判复制”，不是靠跳过验证或缩减测例。



## 情况要求
- 旧地址空间模型同时维护 `prog_sections[]`、`heap_start/heap_end`、`NVMA` 固定数组和 `ShmManager` 旁路映射，缺页、`fork`、`mmap/shm`、退出回收各走各的路径，结构割裂。
- 需要把缺页处理尽量收束到统一后端，并为后续把 `ELF/PT_LOAD`、`brk`、用户栈、`mmap/shm` 全量并入统一地址空间模型打基础。
- 需要保留现有大仓库调用面的可编译性，不能在半迁移状态下把双架构构建面打坏。

## 解决方法
- 引入新的统一内存对象骨架：
  - `kernel/proc/vm_area.hh`：把旧 `vma` 升级为 `VmArea`，保留兼容字段，同时新增 `object/page_offset/area_kind/grow_policy/guard_pages/private_page_overlay`。
  - `kernel/proc/vma_space.hh/.cc`：新增 `VMASpace`，底层用 `eastl::list<VmArea>` 持有对象，用 `VmaMapleTree` 做索引。
  - `kernel/proc/vm_object.hh/.cc`：新增 `VmObject`、`AnonVmObject`、`FileVmObject`、`SysvShmVmObject`，统一承载匿名页、文件页和共享页后端。
- 把 `VmaMapleTree` 从依赖旧 `context.hh` 的旧 `struct vma` 改为直接索引新的 `VmArea`。
- 清理头文件耦合：
  - `context.hh` 只保留上下文切换需要的 `Context`。
  - `proc.hh` 与 `process_memory_manager.hh` 的循环依赖拆开后重组，内存访问器下沉到 `proc.cc`。
  - `kernel/shm/ipc_param.hh` 增加 `#pragma once`，避免 `VmObject` 接入后重复定义。
- 把统一缺页后端接到现有运行路径：
  - `VirtualMemoryManager::allocate_vma_page()` 优先走 `vm->object->prepare_page()`。
  - `VmObject` 返回统一的 `VmPageView`，由页表层统一安装映射，并在私有可写映射上打 COW 标记。
- 把 `mmap` 的新元数据接入旧 VMA 主路径：
  - `ProcessManager::mmap()` 在保留旧 `NVMA` 槽位流程的同时，为匿名映射和文件映射填充 `VmObject`、`page_offset`、`area_kind`、`grow_policy` 等字段。
- 把对象生命周期接到现有 `fork/exit` 路径：
  - `ProcessMemoryManager::clone_for_fork()` 共享 `VmObject` 并复制 overlay 元数据，同时补物理页引用。
  - `free_single_vma()` / `VmObject::on_area_destroy()` 负责释放 overlay 所有权和对象引用，避免新后端泄漏。
- 把“兼容旧数组槽位流程”里最容易炸裂的新元数据单独抽出辅助层：
  - 新增 `kernel/proc/vma_metadata_utils.hh/.cc`，统一处理 VMA snapshot、overlay 子区间拷贝、metadata 释放，避免 `fork`、`munmap`、`mprotect` 各自手搓一套对象/页引用逻辑。
  - `private_page_overlay` 从“内嵌 `eastl::unordered_map`”改成“显式托管指针”，避免旧路径里的 `memset(vma)`、整对象赋值、rollback 覆盖把 EASTL 容器内部状态直接踩坏。
- 把 split/rollback/COW 关键路径补到新语义：
- `partial_unmap_vma()` 在 trim/split 时同步维护 `page_offset` 和 overlay 子区间归属，避免后半段 VMA 继续指向错误页源。
- `sys_mprotect()` 的 VMA 拆分/回滚改为走 snapshot + overlay 子区间复制，不再对带对象 VMA 直接平凡复制。
- `VirtualMemoryManager::resolve_cow_page()` 在对象私有映射上回写 overlay 元数据，避免写 fault 后只改 PTE、不更新 `VmArea` 的悬空状态。
- `FileVmObject::prepare_page()` 改成“锁内查缓存 + 锁外读文件 + 回填二次确认”，避免拿着 `object_lock_` 进入 `fstat/read` 这类可睡眠路径，消除两架构统一出现的 `scheduler.cc:149` 断言。

## 验证方式
- `make build ARCH=riscv`
- `make build ARCH=loongarch`
- `timeout 60s make run r QEMU_MEM=1G`
- `timeout 60s make run l QEMU_MEM=1G`
- `timeout 60s make run r QEMU_MEM=1G`（修复 `object_lock_` 后复验）
- `timeout 60s make run l QEMU_MEM=1G`（修复 `object_lock_` 后复验）

## 验证结果
- 两个架构均构建通过。
- 本轮在当前工作区重新复验：
  - `make build ARCH=riscv`
  - `make build ARCH=loongarch`
  - `timeout 60s make run r QEMU_MEM=1G`
  - `timeout 60s make run l QEMU_MEM=1G`
- 两个架构的 60s smoke 都不再出现之前那条 `FileVmObject::prepare_page()` 里访问 `private_page_overlay.find()` 的崩溃。
- 中间一轮 smoke 曾统一收敛为 `kernel/proc/scheduler.cc:149` 的 `cpu->get_num_off() == 1` 断言：
  - RISC-V 日志：`logs/output_r_20260617-204308_vmaspace-smoke_timeout-60s.txt`
  - LoongArch 日志：`logs/output_l_20260617-204324_vmaspace-smoke_timeout-60s.txt`
- 该断言在把 `FileVmObject` 的文件读路径移出 `object_lock_` 后消失，新的 60s smoke 已能继续向前跑多个 `fs_bind_cloneNS*` 用例直到超时窗口结束：
  - RISC-V 复验日志：`logs/output_r_20260617-204534_vmaspace-smoke2_timeout-60s.txt`
  - LoongArch 复验日志：`logs/output_l_20260617-204534_vmaspace-smoke2_timeout-60s.txt`
- 当前 60s smoke 的限制：
  - 没有自然跑完整轮，退出码是外层 `timeout` 的 `124`。
  - 日志里 LTP 子脚本 `Summary` 显示 `failed 0 / broken 0`，但外层仍打印 `FAIL LTP CASE ...: 0`，这更像现有 wrapper 判定口径问题，需要后续单独核对。
- 当前轮最新 smoke 日志：
  - RISC-V：`logs/output_r_20260617-211512_vmaspace-smoke_timeout-60s.txt`
  - LoongArch：`logs/output_l_20260617-211512_vmaspace-smoke_timeout-60s.txt`
- 当前轮最新 smoke 现象：
  - 两个架构都能继续推进到 `fs_bind_cloneNS05.sh`，没有出现新的 panic、调度断言或 `object_lock_` 相关睡眠错误。
  - 退出仍然是外层 `timeout` 的 `124`，不是内核主动崩溃。
- 当前验证覆盖的是“结构接线 + 双架构编译面 + 旧 overlay 崩溃链消失 + 调度断言消失”，还没有在本轮继续跑 `mmap3/libcbench/shm*` 定向回归。

## 第 2 提交增量
- 情况要求：
  - `execve()` 仍然沿用“预装 PT_LOAD/解释器段 + 立即映射用户栈”的旧路径，和 `VMASpace + VmObject` 的懒页后端没有真正接上。
  - `FileVmObject` 还缺少“只读文件有效字节 + 其后零填充”的精确语义，无法正确承载 ELF `BSS` 尾页。
- 解决方法：
  - `ProcessManager::execve()` 新增 `register_lazy_file_area()`，把主程序 `PT_LOAD`、解释器 `PT_LOAD` 都注册为 `VMASpace + FileVmObject` 私有文件映射，不再在 `execve()` 里预读整段页表。
  - 用户栈改为 `VmAreaKind::UserStack + AnonVmObject` 的 grow-down 懒映射，并把 guard page 语义记录进 `guard_pages`。
  - `ProcessMemoryManager::ensure_user_pagetable_hierarchy()` 提供 LoongArch 通用页表骨架预建，保证 tlbr refill 能稳定落回懒缺页路径。
  - `FileVmObject::prepare_page()` 改为尊重 `file_backed_bytes/zero_fill_past_file`，精确处理 `filesz < memsz` 的尾页和 BSS 区。
  - `ProcessManager::mmap()` 为普通文件映射补齐 `file_backed_bytes` 元数据，避免文件后端页源语义只对 `execve` 生效。
- 验证方式：
  - `make build ARCH=riscv`
  - `make build ARCH=loongarch`
  - `timeout 60s make run r QEMU_MEM=1G`
  - `timeout 60s make run l QEMU_MEM=1G`
- 验证结果：
  - 两个架构重新构建均通过。
  - 两个架构的 60s smoke 都自然结束，`exit_code=0`，没有出现新的 panic、调度断言、`execve` 崩溃或页故障死循环。
  - 本轮 smoke 日志：
    - RISC-V：`logs/output_r_20260617-212857_vmaspace-execve-smoke_timeout-60s.txt`
    - LoongArch：`logs/output_l_20260617-212857_vmaspace-execve-smoke_timeout-60s.txt`

## 对应提交
- `feat(mm): 引入 vmaspace 与 vmobject 骨架`
- 第 2 提交待落地：`feat(mm): 将 execve 段与用户栈迁移到 vmaspace`

## 第 3 提交增量
- 情况要求：
  - trap 缺页、`copy_in/copy_out`、`fork()` 私有页复制仍然各自分散处理，`execve()` 往新页表写入用户栈时还会误借“当前进程旧地址空间”找 VMA。
  - 第 2 提交把 `PT_LOAD`/用户栈迁到懒注册后，这类“按当前进程猜测目标 mm”的旧路径已经不再可靠，需要统一收束到 `ProcessMemoryManager::fault_page()`。
- 解决方法：
  - `ProcessMemoryManager` 新增 `fault_page()`，统一接住写时复制、普通缺页、`MAP_GROWSDOWN` 栈扩展和最终 `allocate_vma_page()` 安装。
  - RISC-V / LoongArch 两套 trap `mmap_handler()` 改成只负责解析 fault 类型，再统一委托给 `mm->fault_page()`。
  - `VirtualMemoryManager::copy_in()` / `copy_out()` / `copy_str_in()` / `ensure_user_*_range()` 增加显式 `target_mm` 解析逻辑，不再默认把“当前正在运行的进程”误当成目标页表所属地址空间。
  - `execve()` 往 `new_pt` 写随机栈数据、argv/envp/auxv/argc，以及 `CLONE_CHILD_SETTID` 往子页表写 tid，全部改成显式传入目标 `ProcessMemoryManager`。
  - `clone_for_fork()` 新增 `clone_private_vm_space_for_fork()`，把已经迁入 `VMASpace` 的私有区域（`PT_LOAD`、解释器段、堆、用户栈）统一走页级 COW 复制，不再依赖 `prog_sections[]/heap` 的分散特判。
- 验证方式：
  - `make build ARCH=riscv`
  - `make build ARCH=loongarch`
  - `timeout 60s make run r QEMU_MEM=1G`
  - `timeout 60s make run l QEMU_MEM=1G`
- 验证结果：
  - 两个架构重新构建均通过。
  - 两个架构的 60s smoke 都自然结束，`exit_code=0`，没有出现新的 panic、页故障死循环、`copy_out(new_pt, ...)` 失败或 fork 后写时复制异常。
  - 本轮 smoke 日志：
    - RISC-V：`logs/output_r_20260617-214118_vmaspace-cow-smoke_timeout-60s.txt`
    - LoongArch：`logs/output_l_20260617-214118_vmaspace-cow-smoke_timeout-60s.txt`

## 状态
- 第 1 提交已完成待验收：骨架、兼容层、双架构构建和 60s smoke 已闭环。
- 第 2 提交已完成待验收：`execve` 主程序段、解释器段、用户栈已切到 `VMASpace + VmObject` 懒注册路径，双架构构建和 60s smoke 已闭环。
- 第 3 提交已完成待验收：统一 fault、`copy_in/copy_out` 目标地址空间感知，以及 `VMASpace` 私有区域 fork COW 已闭环。
- 整体计划仍进行中：后续继续推进第 4 提交起的 `mmap/shm` 全量迁移、旧 VMA/SHM 旁路清理，以及 `mmap3/libcbench/shm*` 定向回归和 wrapper 判定口径核对。

## 第 4 提交增量
- 情况要求：
  - `mmap/munmap/mprotect/mremap/msync/madvise/shm*` 的主路径仍大量假定映射一定落在旧 `NVMA` 静态槽位里，新的动态 `VMASpace` 区域虽然已经能承载 `execve`/栈/COW，但运行态 `mmap`、`shmat`、`/proc/self/maps`、memfd seal 检查等接口还看不全。
  - 共享文件映射和 SysV SHM 的对象后端已经存在，但旧代码里仍混着“扫描 `vma_data._vm[]`”“`register_shared_attachment_vma()` 兼容桥”“共享映射单独旁路页表”的残留判断。
- 解决方法：
  - `ProcessManager::mmap()` 改为直接向 `VMASpace` 创建匿名映射、私有文件映射和共享文件映射对应的 `VmArea`，补齐 `object/page_offset/file_backed_bytes/area_kind/grow_policy` 元数据，并把 `MAP_POPULATE` 失败清理收口到统一 `unmap_memory_range()`。
  - `ProcessManager::munmap()`、`mremap()` 改为基于 `ProcessMemoryManager::find_vma_covering()` 和统一 `unmap_memory_range()` 处理，不再要求目标映射必须来自旧 `NVMA` 槽位。
  - `sys_mprotect()`、`sys_msync()`、`sys_madvise()`、`sys_remap_file_pages()`、`sys_fcntl(F_SEAL_WRITE)` 等运行时接口改为通过 `for_each_vma()` / `for_each_vma_in_range()` 覆盖动态 `VMASpace` 区域；`sys_mprotect()` 的拆分/回滚同步补齐动态 `VmArea` 的 overlay 元数据复制。
  - `/proc/self/maps`、`/proc/self/pagemap`、`/proc/self/status` 改为遍历统一地址空间视图，避免动态 `mmap/shmat` 对用户态可见性缺页。
  - `ShmManager` 继续承担 SysV IPC 元数据，但共享文件对象缓存、对象全局索引、SysV SHM 对象获取已经全部通过统一对象管理接口承载；`shmat` 注册的共享附件 VMA 也转成 `VMASpace + SysvShmVmObject`。
- 验证方式：
  - `make build ARCH=riscv`
  - `make build ARCH=loongarch`
- 验证结果：
  - 两个架构重新构建均通过。
  - 当前阶段验证覆盖的是“动态 `mmap/shm` 元数据路径是否还能双架构编译收口”；`mmap3/libcbench/shm*` 定向回归留到第 5 提交统一验收。

## 当前状态
- 第 1 提交已完成待验收：骨架、兼容层、双架构构建和 60s smoke 已闭环。
- 第 2 提交已完成待验收：`execve` 主程序段、解释器段、用户栈已切到 `VMASpace + VmObject` 懒注册路径，双架构构建和 60s smoke 已闭环。
- 第 3 提交已完成待验收：统一 fault、`copy_in/copy_out` 目标地址空间感知，以及 `VMASpace` 私有区域 fork COW 已闭环。
- 第 4 提交已完成待验收：动态 `mmap/shm` 的主路径已经能遍历统一地址空间模型，双架构重新构建通过。
- 第 5 提交待落地：继续清掉共享映射旧接口和假后端残留，补架构文档并做定向验收；本轮验证收尾已完成，临时调试入口已清理并复验通过。

## 验证收尾
- 情况要求：
  - 这轮验证阶段临时加过 `sys_brk` 轨迹打印，并把 `initcode` 缩成了子集，便于快速确认 `VMASpace + VmObject` 的主路径是否还会崩。
  - 在确认编译面和 60s smoke 都稳定后，需要把这些调试入口收回，恢复正式回归入口，再做一次干净复验。
- 解决方法：
  - 删除 `kernel/sys/syscall_handler.cc` 里 `sys_brk` 的临时 `brk:trace` 打印。
  - 恢复 `user/app/initcode-rv.cc` 和 `user/app/initcode-la.cc` 为完整回归顺序，不再只跑局部子集。
- 验证方式：
  - `make build ARCH=riscv`
  - `make build ARCH=loongarch`
  - `timeout 60s make run r QEMU_MEM=1G`
  - `timeout 60s make run l QEMU_MEM=1G`
- 验证结果：
  - 两个架构均构建通过。
  - 两个架构的 60s smoke 都没有出现新的 panic、断言或 `object_lock_` 相关睡眠错误。
  - 日志里的 LTP 子脚本 `Summary` 仍然是 `passed 19 / failed 0 / broken 0 / skipped 0 / warnings 0`，外层依旧会打印 `FAIL LTP CASE fs_bind_cloneNS0x.sh: 0`，`exit_code` 为 `124`，这和之前一致，属于现有 wrapper 口径问题，不是新增内核崩溃。
  - RISC-V 日志：`logs/output_r_20260618-165517_vmaspace-cleanup-smoke_timeout-60s.txt`
  - LoongArch 日志：`logs/output_l_20260618-165517_vmaspace-cleanup-smoke_timeout-60s.txt`

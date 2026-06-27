# NVMA 兼容层清理计划

## 背景

`VMASpace + VmObject` 主路径已经落地，但当前仓库还没有真正完成“旧 `NVMA` 兼容层清零”。按照本项目“不为兼容保留双套契约”的约束，这个问题需要作为独立计划收口，而不是继续挂在重构完成文档后面。

本计划只关注一件事：把仍然参与运行时语义的 `NVMA/vma_data._vm[]/vma_index` 遗留路径彻底移除，让用户地址空间重新回到 `VMASpace` 单权威。

## 问题现象

- 当前不是“只剩旧字段但已冻结”的状态，而是 `VMASpace` 与 `NVMA/vma_data._vm[]/vma_index` 双权威并存。
- 新建 VMA 的主路径大多已经切到 `VMASpace`，包括 `mmap()`、`shmat()`、`execve()` 懒段、用户栈和 `brk()` 的 heap 元数据。
- 但 legacy 兼容层仍然参与查询、拆分、fork 复制、共享映射回收和调试统计；这意味着地址空间事实来源并不唯一。
- 更危险的是，兼容层不是纯只读遗留，而是仍然存在会继续向 `vma_data._vm[]` 新增条目的活跃写路径；这会让混合态继续扩散。

## 具体位置

### 根状态与对外入口

- `kernel/proc/process_memory_manager.hh`
  - `NVMA = 256`
  - `VMA vma_data`
  - `VmaMapleTree vma_index`
- `kernel/proc/proc.cc`
  - `Pcb::get_vma()`
  - `Pcb::set_vma()`

### 仍在运行时参与语义的路径

- `kernel/proc/process_memory_manager.cc`
  - `find_vma_covering()`
  - `find_first_vma_at_or_after()`
  - `find_prev_vma()`
  - `has_vma_conflict()`
  - `rebuild_vma_index()`
  - `clone_for_fork()`
  - `has_other_shared_backing_fragment()`
  - `partial_unmap_area()`
  - `print_memory_usage()`
  - `get_vma_memory_usage()`
  - `check_memory_leaks()`
- `kernel/sys/syscall_handler.cc`
  - `sys_mprotect()` 中的 `is_legacy_vm` 分支仍会继续分配 legacy 槽位
- `kernel/proc/signal.cc`
  - 本地 `find_vma_covering()` 仍用“指针是否落在 `base + NVMA` 范围内”识别 legacy 条目
  - `expand_writable_stack_vma_for_signal()`
  - `clamp_signal_stack_top_to_writable_vma()`

### 已接近废弃但仍保留接口的遗留 API

- `kernel/proc/process_memory_manager.hh/.cc`
  - `free_single_vma(int)`
  - `partial_unmap_vma(int, ...)`
  - `find_overlapping_vmas(...)`
  - `is_vma_valid(int)`

## 移除遗留问题的计划

### 第一阶段：切断 legacy 对外暴露

- 删除或废弃 `Pcb::get_vma()/set_vma()`。
- 清理按槽位索引设计的 legacy 接口，避免新的调用点继续依赖 `NVMA` 模型。
- 目标：调用面只能通过 `ProcessMemoryManager` 的区间语义接口访问 VMA。

### 第二阶段：删除活跃写路径

- 删除 `sys_mprotect()` 的 `is_legacy_vm` 分支，统一收口到 `VMASpace` 动态 split。
- 删除 `partial_unmap_area()` 的 `legacy_area` 分支，统一走 `vm_space.create_area()` 打洞。
- 目标：运行中的地址空间不再新增 legacy 槽位。

### 第三阶段：收口到单权威查询

- 去掉 `find_vma_covering()/find_first_vma_at_or_after()/find_prev_vma()/has_vma_conflict()` 对 `vma_index` 的混查。
- 去掉 `has_other_shared_backing_fragment()` 对 `vma_data._vm[]` 的扫描。
- 缩减或删除 `rebuild_vma_index()` 的 legacy 职责。
- 目标：查询、冲突检测、共享映射回收都只承认 `VMASpace` 一套事实来源。

### 第四阶段：删除 legacy 复制与调试镜像

- 从 `clone_for_fork()` 删除 `vma_data._vm[]` 复制逻辑。
- 把 `print_memory_usage()`、`get_vma_memory_usage()`、`check_memory_leaks()` 改成遍历统一 VMA 视图。
- 继续评估 `prog_sections[]`、`heap_start/heap_end` 是否也应随之收口。
- 目标：fork、统计、排障输出全部基于 `VMASpace`。

## 验收标准

- 不再存在任何会向 `vma_data._vm[]` 新增条目的运行时路径。
- `find/prev/next/conflict/gap` 相关查询只以 `VMASpace` 为权威。
- `fork`、`mprotect`、`munmap`、`mremap`、`shmat/shmdt`、信号栈扩展都不再依赖 legacy 槽位判断。
- `make build ARCH=riscv` 与 `make build ARCH=loongarch` 均通过。
- 定向回归至少覆盖：`mprotect`、`munmap`、`mremap`、`shmat/shmdt`、`fork`、`pthread`、`signal frame`、`/proc/self/maps`。

## 当前状态

待执行。当前可以判断为“VMA 主路径已迁移，但 `NVMA` 兼容层仍在运行时参与语义”，不能视为兼容层已清零。

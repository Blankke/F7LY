# VMASpace + VmObject + COW 重构记录

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

## 对应提交
- 待用户确认后提交

## 状态
- 第 1 提交已完成待验收：骨架、兼容层、双架构构建和 60s smoke 已闭环。
- 整体计划仍进行中：后续继续推进第 2 提交起的 `ELF/PT_LOAD + brk + 用户栈` 迁移，以及 `mmap3/libcbench/shm*` 定向回归和 wrapper 判定口径核对。

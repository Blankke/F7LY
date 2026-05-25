# 项目架构导航

## 文档概况

本文档给 agent 快速建立 F7LY 的架构地图。它说明各模块“负责什么、依赖什么、改动时先看哪里”，不追逐每个函数细节。需要命令和运行方法时读 `agent_docs/development_debugging.md`；需要评测进度时读 `agent_docs/scoreboard.md`。

## 总体分层

F7LY 可以按五层理解：

1. 架构启动与硬件抽象：`kernel/boot/`、`kernel/hal/`、`kernel/trap/`、`kernel/devs/`
2. 内存与地址空间：`kernel/mem/`、`kernel/shm/`
3. 进程、线程与调度：`kernel/proc/`
4. Linux ABI 与系统调用：`kernel/sys/`
5. 文件系统、设备文件、网络和用户态回归：`kernel/fs/`、`kernel/net/`、`user/`

通用代码放在公共目录；架构差异放在 `riscv/`、`loongarch/` 子目录。改动时先判断问题属于“架构差异”还是“通用 Linux 语义”，不要把架构 workaround 写进通用路径。

## 顶层目录职责

- `kernel/`：内核主体。
- `user/`：用户态 initcode、系统调用封装、回归测试入口。
- `thirdparty/EASTL/`：内核使用的 EASTL 容器库。
- `busybox/`：预置 BusyBox 二进制，按架构和 libc 区分。
- `images/`：本地运行镜像、initrd、sdcard 备份等大文件资产。
- `scripts/`：挂载、镜像恢复、宿主机辅助脚本。
- `debug/gdb/`：按架构拆分的 GDB 调试配置。
- `tools/ltp/`：LTP 输出解析、排名、历史 scoreboard 工具。
- `scoreboard/`：当前四组合 Markdown scoreboard。
- `ref/ltp/`：上游 LTP 参考源码，当前应 checkout 到 `20240524` tag。
- `ref/testsuits-for-oskernel/`：basic、busybox、lua、libcbench 等非 LTP 测例参考源码。
- `docs/archive/`、`docs/dev-notes/`：历史材料和排障记录；当前事实仍以源码和 Git 记录为准。

## 启动与架构入口

RISC-V：

- 入口：`kernel/boot/riscv/entry.S`
- 早期启动：`kernel/boot/riscv/start.cc`
- 主初始化：`kernel/boot/riscv/main.cc`
- trap：`kernel/trap/riscv/trap.cc`
- 页表：`kernel/mem/riscv/`
- virtio disk：`kernel/fs/drivers/riscv/virtio_disk2.cc`

LoongArch：

- 入口：`kernel/boot/loongarch/entry.S`
- 主初始化：`kernel/boot/loongarch/main.cc`
- trap：`kernel/trap/loongarch/trap.cc`
- TLB refill/uservec：`kernel/trap/loongarch/`
- 页表：`kernel/mem/loongarch/`
- virtio/pci：`kernel/devs/loongarch/` 与 `kernel/fs/drivers/loongarch/`

LoongArch 的敏感点是高地址内核映射、TLB refill、trapframe 动态映射、用户态 LL/SC 保留窗口。涉及 pthread、futex、信号、clone、mmap 时优先考虑这些交界。

## 内存管理

核心模块：

- 物理内存：`kernel/mem/physical_memory_manager.*`
- 内核堆：`kernel/mem/heap_memory_manager.*`
- 页表与映射：`kernel/mem/virtual_memory_manager.*` 和架构页表目录
- 用户地址空间：`kernel/proc/process_memory_manager.*`
- mmap 元数据：`kernel/proc/context.hh` 的 `struct vma`
- 共享内存：`kernel/shm/shm_manager.*`

关键思路：

- PMM 用 buddy 管理页级物理页。
- HMM 在 PMM 切出的 heap 区域上做细粒度分配，全局 `new/delete` 走这里。
- `ProcessMemoryManager` 是用户地址空间权威所有者，统一管理 ELF 段、heap、mmap VMA、用户页表和引用计数。
- `CLONE_VM` 共享 mm；普通 fork 深拷贝 mm；exec 成功后替换 mm。
- `allocate_vma_page()` 是 mmap 懒分配统一入口，修改 mmap/munmap/mremap 时必须同时检查 trap 缺页、`copy_in/copy_out` 懒分配和退出释放。

## 进程、线程与调度

核心模块：

- PCB：`kernel/proc/proc.hh`
- 进程池与生命周期：`kernel/proc/proc_manager.cc`
- 调度：`kernel/proc/scheduler.cc`
- 上下文切换：`kernel/proc/<arch>/swtch.S`
- futex：`kernel/proc/futex.cc`
- 信号：`kernel/proc/signal.cc`
- pipe/FIFO：`kernel/proc/pipe.cc`、`kernel/fs/vfs/fifo_manager.cc`

关键不变量：

- `Pcb::_lock` 保护单个 PCB 状态；`ProcessManager::_wait_lock` 保护父子等待和 reparent。
- `_pid` 是进程 ID，`_tid` 是线程 ID，`_tgid` 是线程组 ID。
- `CLONE_THREAD` 共享 TGID；`CLONE_VM` 共享 mm；`CLONE_FILES` 共享 fd 表；`CLONE_SIGHAND` 共享 signal handler 表。
- `CLONE_CHILD_CLEARTID` 和 `set_tid_address` 退出时写 4 字节 pid_t 零，并 futex wake，不要写 8 字节。
- `wait4()` wait status 使用 Linux 编码：正常退出 `(exit_code & 0xff) << 8`，信号退出低 7 位存信号。

## Linux ABI 与系统调用

核心模块：

- 编号：`kernel/sys/syscall_defs.hh`
- 声明：`kernel/sys/syscall_handler.hh`
- 分发与实现：`kernel/sys/syscall_handler.cc`
- 用户态封装：`user/syscall_lib/`

关键思路：

- 系统调用号大体沿用 asm-generic Linux 编号。
- `SyscallHandler::init()` 先把分发表填成默认 ENOSYS，再用 `BIND_SYSCALL(name)` 注册。
- syscall 参数从当前进程 trapframe 的 `a0..a5` 读取，syscall number 在 `a7`。
- 内核侧返回负 Linux errno，例如 `-EINVAL`、`-EFAULT`。
- 新增 syscall 需要同步 defs、handler 声明、handler 实现、绑定列表，必要时同步用户态 wrapper 和测例入口。
- `syscall_handler.cc` 很大，修改前先定位同类 syscall 的参数校验、用户拷贝、fd 检查和错误码风格。

## Trap、中断与用户态返回

RISC-V：

- 用户 syscall cause 为 8，`epc += 4` 后进 syscall handler。
- page fault cause 12/13/15 走 mmap 懒分配，失败发 SIGSEGV。
- timer tick 推进 ticks、唤醒 sleep、检查 POSIX timer 和 interval timer。

LoongArch：

- syscall ecode 为 `0xb`，`era += 4` 后进 syscall handler。
- page fault ecode `0x1..0x7`；地址错误 `0x8/0x9` 直接 SIGSEGV。
- `ESTAT` 同时包含异常编码和中断 pending，`devintr()` 必须先确认 ecode 为 0 再分发中断。
- 对“PTE 合法但 TLB 残留旧状态”的用户页，先按页失效 TLB 重试。
- `usertrapret()` 会按当前线程 trapframe 动态重映射 TRAPFRAME，并尽量减少 TLB 失效，避免破坏 LL/SC。

## 文件系统与文件对象

核心模块：

- VFS 工具：`kernel/fs/vfs/vfs_utils.cc`
- ext4 封装：`kernel/fs/vfs/vfs_ext4_ext.cc`、`kernel/fs/vfs/vfs_ext4_blockdev_ext.cc`
- lwext4：`kernel/fs/lwext4/`
- file 抽象：`kernel/fs/vfs/file/`
- 虚拟文件系统：`kernel/fs/vfs/virtual_fs.*`
- 块缓存：`kernel/fs/bio.cc`

关键思路：

- 根文件系统优先使用主盘 ext4；若主盘 FAT32 且 initrd 是 ext4，则用 initrd 作为根，并把 FAT32 挂到 `/fat32`。
- `filesystem_init()` 在第一个进程上下文中运行，因为挂载可能 sleep。
- 新式 `fs::file` 是主要 file 对象抽象，派生类包括 normal、directory、device、pipe、socket、virtual、fat32、epoll。
- old `struct file` 与 new `fs::file` 并存，修改 fd 路径前先确认实际对象类型。
- ext4 全局 `extlock` 允许同进程递归进入，用于串行化 lwext4 状态。
- `/proc`、`/etc`、`/dev` 下大量内容由 `VirtualFileSystem` 注册。

## ELF 装载与用户态回归

核心路径：

- exec：`ProcessManager::execve()`
- 用户入口：`user/app/initcode-rv.cc`、`user/app/initcode-la.cc`
- 回归调度：`user/user_lib/user_test.cc`
- 用户 syscall ABI：`user/syscall_lib/arch/<arch>/syscall_arch.hh`

exec 关键思路：

- 支持 shebang，重写成解释器路径加脚本路径。
- 支持 `PT_INTERP`，会重写 musl/glibc 动态链接器路径。
- 加载 ELF `PT_LOAD` 时按 `p_align` 对齐；LoongArch 16K 对齐段不能退化成 4K。
- 装载成功后关闭 CLOEXEC fd，替换 mm，设置 trapframe PC/SP。

回归关键思路：

- initcode 不是 shell，而是直接调用 `regression_suite_4d1444()`。
- `run_test()` 负责 fork、子进程 setpgid、execve、父进程 waitpid 和 PASS/FAIL 输出。
- 每个测例独立进程组，避免 LTP `kill(0, SIGKILL)` 误杀 init。
- 当前 scoreboard 记录磁盘测例结构和人工/agent 协作状态，不等同于运行日志。

## 修改时的定位规则

- syscall 语义问题：先看 `syscall_handler.cc`，再看 proc/fs/mem 具体后端。
- exec/动态链接问题：先看 `ProcessManager::execve()`、ELF auxv、loader 路径重写。
- mmap/缺页问题：同时看 syscall mmap、VMA 元数据、trap page fault、`copy_in/copy_out`。
- fork/clone/pthread 问题：同时看 clone flags、mm/file/sighand 共享、futex、signal、架构 usertrapret。
- 文件语义问题：先确认是虚拟文件、设备文件、普通 ext4 文件、pipe/socket 还是 old file 路径。
- LoongArch 独有问题：优先检查 TLB、DMWIN、PTE_MAT、trapframe 动态映射、LL/SC 相关补丁。

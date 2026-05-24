# F7LY 项目级 Agent 上手说明

本文档用于让新的 AI 窗口快速理解当前仓库，不需要每次从头通读全部代码。
它记录的是当前代码事实、主要架构思想、运行方式和调试入口，不是未来修改计划。

## 沟通与协作约定

- 默认使用中文回答。
- 修改代码前先看最近 Git 历史和当前工作区状态，避免覆盖人类或其他 AI 的改动。
- 不要在未明确要求时提交 commit，不要推送，不要重置历史。
- 本项目是 OS Demo/评测项目，但实现时仍优先选择清晰、可维护、可扩展的方案。
- 不要为了兼容旧接口保留双套逻辑；需求升级时应同步更新所有调用方并删除旧入口。
- Python 命令必须在项目目录对应 venv 中执行；若没有 venv，使用 `uv venv` 创建并激活，执行前用 `which python` 或 `python -c 'import sys; print(sys.executable)'` 确认解释器。
- 分析 jsonl/csv/xlsx 时只打印字段名、长度和前 N 字符，避免把长文本刷进上下文。
- 任务总结在聊天里完成，不要额外创建总结类 `.md` 文件。`README.md`、`AGENTS.md` 等项目长期文档除外。

## 项目定位

F7LY OS 是一个基于 xv6 思路扩展的教学/比赛用内核，核心目标已经从基本启动演进到支持 Linux ABI、BusyBox、glibc/musl 程序和 LTP 回归评测。

当前主线能力：

- 双架构：RISC-V 与 LoongArch。
- C++23 freestanding 内核，禁用异常和 RTTI。
- 面向 Linux ABI 的系统调用号和用户态调用约定。
- ext4 根文件系统为主，保留 FAT32 数据盘支持。
- 支持动态 ELF 装载、musl/glibc 动态链接器路径重写、shebang 脚本入口。
- 支持进程、线程、clone/clone3、futex、信号、mmap、共享内存、POSIX timer、epoll/eventfd/memfd 等大量 LTP 相关接口。
- 用户态 `initcode` 不是传统 shell，而是直接运行回归套件，然后调用 `shutdown()`。

## 构建与运行

常用命令：

```bash
make all
make build ARCH=riscv
make build ARCH=loongarch
make run r
make run l
make debug ARCH=riscv
make debug ARCH=loongarch
make clean
```

架构别名：

```bash
make r
make l
make run r
make run l
```

运行内核时不要直接把 QEMU 长输出刷进聊天上下文。统一使用 `make run r` 或 `make run l`，并把输出写入带时间戳和运行信息的文本文件，方便后续对比。完整测试 timeout 统一设为 40 分钟；单条测例 timeout 最多 5 分钟。

RISC-V 运行日志示例：

```bash
ts=$(date +%Y%m%d-%H%M%S)
log="output_r_${ts}_make-run-r_QEMU_MEM-1G_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "cmd=timeout 40m make run r QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run r QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

LoongArch 运行日志示例：

```bash
ts=$(date +%Y%m%d-%H%M%S)
log="output_l_${ts}_make-run-l_QEMU_MEM-1G_timeout-40m.txt"
{
  echo "run_at=${ts}"
  echo "arch=loongarch"
  echo "cmd=timeout 40m make run l QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 40m make run l QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

日志文件命名使用 `output_*.txt`，已被 `.gitignore` 覆盖。任务汇报时只说日志文件路径、退出码和关键现象，不要把完整 QEMU 日志贴进聊天。

调试单条测例时，先在 `user/user_lib/user_test.cc` 或 `user/app/initcode-*.cc` 中只打开目标测例，再运行 5 分钟 timeout 的日志命令。例如：

```bash
ts=$(date +%Y%m%d-%H%M%S)
log="output_r_${ts}_single-target_QEMU_MEM-1G_timeout-5m.txt"
{
  echo "run_at=${ts}"
  echo "arch=riscv"
  echo "scope=single-target"
  echo "cmd=timeout 5m make run r QEMU_MEM=1G"
  echo "git_branch=$(git branch --show-current 2>/dev/null || true)"
  echo "git_head=$(git rev-parse --short HEAD 2>/dev/null || true)"
  echo "---- output ----"
  timeout 5m make run r QEMU_MEM=1G
  echo "exit_code=$?"
} > "$log" 2>&1
echo "$log"
```

LoongArch 单条测例同理把日志名前缀改成 `output_l_`，命令改成 `timeout 5m make run l QEMU_MEM=1G`。调试时一定不要每次都跑完整测试；修哪个测例就先只跑哪个测例，确认后再跑更大范围回归。

实际 `Makefile` 规则：

- 默认 `ARCH ?= riscv`。
- `make r` 或目标名包含 `riscv` 时切到 RISC-V。
- `make l` 或目标名包含 `loongarch` 时切到 LoongArch。
- RISC-V 工具链前缀：`riscv64-linux-gnu-`。
- LoongArch 工具链前缀：`loongarch64-linux-gnu-`。
- 输出目录：`build/riscv/` 或 `build/loongarch/`。
- RISC-V 内核 ELF 输出：`build/riscv/kernel-qemu`。
- LoongArch 内核 ELF 输出：`build/loongarch/kernel-la`。
- `initcode` 会和内核一起编译：
  - RISC-V：`user/app/initcode-rv.cc` -> `user/initcode-rv`
  - LoongArch：`user/app/initcode-la.cc` -> `user/initcode-la`
- EASTL 会通过 `thirdparty/EASTL/Makefile` 编译成 `build/<arch>/thirdparty/EASTL/libeastl.a`。
- LoongArch 运行目标依赖 `prepare-loongarch-image`，会执行 `tools/patch_loongarch_libctest_llsc.sh images/sdcard-la.img`。

QEMU 运行参数：

- RISC-V：`qemu-system-riscv64 -machine virt -kernel build/riscv/kernel-qemu -drive file=images/sdcard-rv.img ... -initrd images/initrd.img`
- LoongArch：`qemu-system-loongarch64 -machine virt -cpu la464-loongarch-cpu -kernel build/loongarch/kernel-la -drive file=images/sdcard-la.img ... -initrd images/initrd.img`
- 默认内存 `QEMU_MEM ?= 1G`，调试内存 `QEMU_DEBUG_MEM ?= 1G`。

调试入口：

```bash
gdb-multiarch -x debug/gdb/riscv.gdb
loongarch64-linux-gnu-gdb -x debug/gdb/loongarch.gdb
```

镜像挂载脚本：

- `scripts/mount/mount-rv.sh`：挂载 `images/sdcard-rv.img` 到 `/mnt/sdcard-rv`，并把 musl loader 链到 `/lib/ld-musl-riscv64.so.1`。
- `scripts/mount/mount-la.sh`：挂载 `images/sdcard-la.img` 到 `/mnt/sdcard-la`。
- `scripts/mount/mount-all.sh`：批量挂载多个镜像，并创建 RISC-V musl loader 链接。
- `scripts/images/restore-sdcards.sh`：从 `images/sdcard-bak/` 恢复当前 `images/sdcard-la.img` 和 `images/sdcard-rv.img`；会覆盖当前镜像，运行前必须确认。

## 顶层目录职责

- `kernel/`：内核主体。
- `user/`：用户态 initcode、系统调用封装和回归测试入口。
- `thirdparty/EASTL/`：内核使用的 EASTL 容器库。
- `busybox/`：预置 busybox 二进制，按架构和 libc 区分。
- `tools/ltp/judge/`：LTP 输出解析、排名和分析脚本。
- `tools/ltp/scoreboard/`：LTP scoreboard 解析工具。
- `tools/`：镜像补丁等辅助工具。
- `images/`：本地运行镜像、initrd、sdcard 备份等大文件资产。
- `scripts/`：挂载、镜像恢复、宿主机辅助运行脚本。
- `debug/gdb/`：按架构拆分的 GDB 调试配置。
- `logs/legacy/`：历史 QEMU 输出样例，新的长日志仍建议用 `output_*.txt` 命名并保持忽略。
- `docs/archive/`：历史设计文档、答辩材料和比赛总结。
- `docs/dev-notes/`：历史排障记录，适合补背景，但当前事实以源码和 Git 记录为准。
- `docs/report-src/`：Typst 文档撰写源文件与配套图片。
- `docs/assets/`：README 等长期文档使用的图片资源。

## 内核模块总览

`kernel/` 采用“通用模块 + 架构子目录”的组织方式：

- `boot/`：架构启动入口和 `main()` 初始化流程。
- `hal/`：CPU/CSR/上下文抽象，包含 `hal/riscv/` 和 `hal/loongarch/`。
- `trap/`：异常、中断、系统调用入口、用户态返回、架构向量汇编。
- `mem/`：物理内存、页表、虚拟内存、内核堆、slab、用户空间拷贝。
- `proc/`：PCB、进程池、调度、clone/fork/exec/wait/exit、futex、signal、pipe、rlimit、posix timer。
- `sys/`：Linux ABI 系统调用号、系统调用分发表和具体 syscall 实现。
- `fs/`：VFS、ext4/lwext4、FAT32、虚拟文件、文件对象、块缓存。
- `devs/`：UART、console、virtio disk、ramdisk、loop、DTB、设备管理器。
- `tm/`：时间、tick、sleep、clock_gettime 等接口。
- `shm/`：SysV shared memory 后端和 attach/detach 管理。
- `net/`：VirtIO Net 适配和 ONPS 协议栈集成，当前不是默认启动主线。
- `libs/`：打印、字符串、C++ ABI、全局 new/delete、锁、信号量、qsort 等内核基础库。

## 构建系统细节

`Makefile` 是当前构建事实的权威入口：

- `MAKEFLAGS += -j$(nproc)` 默认并行编译。
- 内核使用 `-ffreestanding -O2 -fno-builtin -fno-stack-protector -nostdlib -static -fno-exceptions -fno-rtti`，C++ 标准是 C++23。
- `ARCH_DIRS` 包含 `boot/<arch>`、`hal/<arch>`、`link/<arch>`、`mem/<arch>`、`proc/<arch>`、`trap/<arch>`、`devs/<arch>`。
- `COMMON_DIRS` 包含 `libs`、`tm`、`sys`、`shm`。
- `fs/` 和 `net/` 会递归收集所有 C/C++/汇编源文件，不按架构拆分。
- `proc_manager.cc` 和 `syscall_handler.cc` 有局部 `-Wno-error=uninitialized`，目的是规避 EASTL 模板误报，不是全局关闭 `-Werror`。
- EASTL 单独通过 `thirdparty/EASTL/Makefile` 编成 `build/<arch>/thirdparty/EASTL/libeastl.a`。
- `initcode` 由 `user/app/initcode-*.cc`、`user/syscall_lib/syscall.cc`、`user/syscall_lib/printf.cc`、`user/user_lib/user_test.cc` 链成 `build/<arch>/initcode.elf`，再 objcopy 到 `user/initcode-rv` 或 `user/initcode-la`。
- `clean` 会删除 `build/`、所有 `.o/.d`、`thirdparty/EASTL` 产物和 `user/initcode-*`，不是轻量清理。

链接脚本：

- RISC-V `kernel/link/riscv/kernel.ld` 把内核放在 `0x80200000`，`.text` 内依次放普通代码、`trampsec`、`sigtrampsec`。
- LoongArch `kernel/link/loongarch/kernel.ld` 把虚拟链接地址放在 `0x9000000000200000`，物理加载地址 `AT(0x200000)`，并在 `.text` 中按页对齐放 `_keentry`、`_ueentry`、`_tlbrentry`、`_merrentry`、`_sig_trampoline`。
- 两个链接脚本都丢弃 `.note.gnu.build-id` 和 `.comment`，避免 raw binary 生成和高地址布局出问题。

## 启动流程

RISC-V：

1. `kernel/boot/riscv/entry.S` 进入 `_entry`。
2. `kernel/boot/riscv/start.cc` 关闭/配置早期分页和 CSR，然后进入 `main(hartid, dtb_addr)`。
3. `kernel/boot/riscv/main.cc` 保存 DTB，初始化打印、中断、PLIC、进程池、PMM/VMM/HMM、共享内存、slab、标准设备、timer、syscall、用户 init、virtio disk、VFS、FIFO、loop，最后启动调度器。
4. 调度器首次切到 init 进程后进入 `ProcessManager::fork_ret()`，这里完成 `filesystem_init()`、标准 fd 0/1/2、UART 重定向，然后 `usertrapret()` 返回用户态。

LoongArch：

1. `kernel/boot/loongarch/entry.S` 进入 `_entry`，链接脚本把内核放在高地址 `0x9000000000200000`，物理加载地址 `0x200000`。
2. `kernel/boot/loongarch/main.cc` 初始化打印，使用 `DtbManager::find_dtb_and_initrd()` 定位 DTB/initrd，再初始化 APIC、ExtIOI、trap、进程池、内存、标准设备、timer、syscall、用户 init、virtio、VFS、FIFO、loop、调度器。
3. LoongArch 的页表、TLB refill、trapframe 动态映射和用户态 LL/SC 语义是当前最敏感区域。

启动顺序中几个关键点：

- `k_printer.init()` 早于大多数日志输出，内部初始化 `dev::kConsole`。
- `proc::k_pm.init()` 必须早于 `mem::k_vmm.init()`，因为 VMM 初始化会为进程池映射内核栈。
- `proc::k_pm.user_init()` 创建第一个用户进程，但文件系统初始化延后到第一次 `fork_ret()`，因为它可能 sleep。
- 文件系统表、buffer cache、inode table、VFS 虚拟目录和 FIFO 管理器在 `main()` 中初始化，但根文件系统挂载在 `fork_ret()` 的 `filesystem_init()` 中完成。

## 内存管理思想

物理内存：

- `PhysicalMemoryManager` 使用 `BuddySystem` 管理页级物理页。
- `pa_start` 从链接脚本 `end` 之后页对齐开始，前 `BSSIZE` 页用于 buddy 元数据。
- RISC-V 会用 DTB 地址限制可用物理内存上界，避免踩 DTB。
- LoongArch 会读取 DTB memory region，把包含内核镜像的低端连续区交给页分配器，把可用高端连续区切给 kernel heap 和 shm。
- `kmalloc(size)` 当前按页数从 buddy 分配并整段清零；C++ 对象和 EASTL 容器主要走 `HeapMemoryManager` 的细粒度分配器。

内核堆：

- `HeapMemoryManager` 在 PMM 切出的 heap 区域上再放一个 buddy。
- 细粒度分配由 `liballoc_allocator` 承接，全局 `new/delete` 走这里。
- 不能把普通 C++ 对象当整页分配释放，否则非页对齐释放会崩。

虚拟内存：

- `VirtualMemoryManager::init()` 创建内核页表 `k_pagetable`，映射所有进程内核栈，并写入架构寄存器：
  - RISC-V：`satp` + `sfence_vma`
  - LoongArch：`pgdl`、`pwcl`、`pwch`、`tlbinit`
- `map_pages()` 是页表映射核心，RISC-V 需要 `PA2PTE(PGROUNDDOWN(riscv::virt_to_phy_address(pa)))`，LoongArch 需要注意 DMWIN 地址和纯物理地址转换。
- `copy_in/copy_out/copy_str_in` 会处理用户地址访问，读用户页时允许对合法 VMA 做一次懒分配。
- `allocate_vma_page()` 是 mmap 缺页补页统一入口。

进程地址空间：

- 当前核心抽象是 `ProcessMemoryManager`，由 PCB 持有。
- 它统一管理：
  - `prog_sections[]`：ELF 段、动态链接器段、用户栈等。
  - `heap_start/heap_end`：brk/sbrk 堆。
  - `mmap_cursor` 和 `vma_data`：mmap 区和懒分配元数据。
  - `pagetable`：用户页表。
  - 原子 `ref_count`：线程共享地址空间时使用。
- `share_for_thread()` 用于 `CLONE_VM` 线程共享 mm。
- `clone_for_fork()` 用于普通 fork/clone 深拷贝地址空间。
- `cleanup_memory_manager()` 和 `free_all_memory()` 是退出/exec 替换时的权威释放路径，避免资源释放散落。

LoongArch 内存注意事项：

- `memlayout.hh` 中 `DMWIN_MASK`、`VIRT2PHY`、`to_vir/to_phy` 相关逻辑很关键。
- 用户页 PTE 默认应保持可缓存 `PTE_MAT`，否则用户态 LL/SC 原子可能长期失败。
- TRAPFRAME 在 LoongArch 多线程下会在 `usertrapret()` 按当前线程 trapframe 物理页动态重映射，并只在必要时按页对失效 TLB。

页表和地址空间常量：

- `kernel/mem/memlayout.hh` 定义内核栈、TRAMPOLINE、SIG_TRAMPOLINE、TRAPFRAME 位置。
- 内核栈为 `KSTACK_PAGES = 2`，另有 `KSTACK_GUARD_PAGES = 1`，`KSTACK(p)` 按进程池 global id 往低地址排布。
- RISC-V 的 `TRAMPOLINE = (MAXVA >> 1) - PGSIZE`，`SIG_TRAMPOLINE` 在其下一页，`TRAPFRAME` 再下一页。
- LoongArch 没有 RISC-V 同款 trampoline 页，`TRAPFRAME = (MAXVA >> 1) - PGSIZE`，`SIG_TRAMPOLINE` 在其下一页。
- `kernel/proc/trapframe.hh` 中两套 `TrapFrame` 布局不同：RISC-V 保存 `kernel_satp/kernel_sp/kernel_trap/epc/kernel_hartid` 和通用寄存器；LoongArch 保存 `era/kernel_pgdl` 等字段。

VMA 和 mmap：

- `kernel/proc/context.hh` 的 `struct vma` 是 mmap 的核心元数据，字段包括 `used/addr/len/prot/flags/vfd/vfile/offset/max_len/is_expandable/backing_kind/backing_shmid/backing_base`。
- `backing_kind` 有 `VMA_BACKING_NONE`、`VMA_BACKING_FILE`、`VMA_BACKING_SHM`，用于区分匿名映射、普通文件映射和共享内存后端。
- `ProcessManager::mmap()` 负责参数校验、选择地址、记录 VMA、处理文件引用和共享段；真正的页面分配通常延后到缺页。
- `VirtualMemoryManager::allocate_vma_page()` 是 mmap 懒分配统一入口，会按 `prot`、`flags`、文件偏移、共享/私有语义补页。
- `ProcessMemoryManager::unmap_memory_range()`/`unmap_memory_range_fix()` 负责 `munmap` 的 VMA 拆分、释放、写回和引用清理。
- 修改 mmap/munmap/mremap 时要同时检查 trap 缺页路径、`copy_in/copy_out` 懒分配路径和 `free_all_memory()` 退出释放路径。

## 进程、线程和调度

PCB 定义在 `kernel/proc/proc.hh`。

关键字段：

- 状态：`UNUSED`、`USED`、`SLEEPING`、`RUNNABLE`、`RUNNING`、`ZOMBIE`。
- 标识：`_pid`、`_tid`、`_ppid`、`_pgid`、`_tgid`、`_sid`。
- 身份：`_uid/_euid/_suid/_fsuid`、`_gid/_egid/_sgid/_fsgid`。
- 调度：`_slot`、`_priority`、`_cpu_mask`。
- 内存：`_kstack`、`_trapframe`、`ProcessMemoryManager *_memory_manager`。
- I/O：`_cwd`、`_cwd_name`、`ofile *_ofile`、`_umask`、`_personality`。
- 线程同步：`_futex_addr`、`_clear_tid_addr`、`_robust_list`、`_vfork_parent`。
- 信号：`_sigactions`、`_sigmask`、`_signal`、`sig_frame`、`_alt_stack`。
- 统计：`_utime/_stime/_cutime/_cstime`、`_start_time`、`_itimer[]`。

进程池：

- `num_process = 90`。
- `alloc_proc()` 使用轮转策略从 `k_proc_pool` 中找 `UNUSED` PCB。
- 分配时初始化 pid/tid、trapframe、上下文、ofile、sighand、rlimit、计时字段。
- `freeproc_creation_failed()` 是创建失败专用清理路径。
- `freeproc()` 只回收 PCB 字段，真实内存/ofile/sighand 释放应已在 exit 路径完成。

调度：

- `Scheduler::start_schedule()` 无限扫描 `k_proc_pool`。
- 当前策略是优先调度 nice 值最小的 RUNNABLE 进程，再在同优先级中顺序扫描。
- `yield()` 把当前进程改回 RUNNABLE，然后 `call_sched()` 汇编切换。
- `swtch` 位于 `kernel/proc/<arch>/swtch.S`。

clone/fork：

- `sys_clone` 最终走 `ProcessManager::clone()` 和 `fork()`。
- `CLONE_THREAD` 时线程共享 PID/TGID 语义，返回新 tid。
- `CLONE_VM` 共享 `ProcessMemoryManager`。
- `CLONE_FILES` 共享 ofile 表并增加 `_shared_ref_cnt`。
- `CLONE_SIGHAND` 共享 sighand 结构并增加引用。
- `CLONE_VFORK` 使用 `_vfork_parent` 阻塞父进程，直到子进程 exec 或 exit 释放共享地址空间。
- `CLONE_CHILD_CLEARTID` 退出时写 4 字节 pid_t 零，并执行 futex wake。
- `CLONE_PARENT_SETTID` 和 `CLONE_CHILD_SETTID` 都必须按 4 字节 pid_t 写，不要写 8 字节。

退出：

- `exit()` 只退出当前线程。
- `exit_group()` 标记同 TGID 线程 killed 并唤醒睡眠线程，然后当前线程退出。
- `exit_proc()` 先关中断，处理 reparent、clear_tid、资源释放，再置 ZOMBIE 并唤醒父进程。
- `wait4()` 回收 ZOMBIE，写回 Linux wait status 编码。

进程生命周期关键不变量：

- `Pcb::_lock` 保护单个 PCB 状态；`ProcessManager::_wait_lock` 保护父子等待和 reparent；调度器切换前要求当前进程锁已持有。
- `Pcb::_pid` 是进程 ID，`_tid` 是线程 ID，`_tgid` 是线程组 ID；`CLONE_THREAD` 新线程共享 TGID。
- `ofile` 里有 `_shared_ref_cnt` 和 `_fl_cloexec[]`；`CLONE_FILES` 共享 fd 表，普通 fork 复制引用，exec 成功后关闭 CLOEXEC fd。
- `sighand_struct` 有 `refcnt`；`CLONE_SIGHAND` 共享信号处理表，信号 mask 和 pending signal 仍在 PCB 上。
- `ProcessMemoryManager` 的引用计数用 EASTL atomic；`CLONE_VM` 走 `share_for_thread()`，普通 fork 走 `clone_for_fork()`。
- `freeproc_creation_failed()` 专门给创建失败回滚使用；不要把它和正常 `exit_proc()`/`freeproc()` 混用。
- `CLONE_CHILD_CLEARTID` 和 `set_tid_address` 相关写用户地址时应写 4 字节 pid_t 零，然后 futex wake；不要写 8 字节。
- `CLONE_VFORK` 通过 `_vfork_parent` 阻塞父进程，子进程 exec 或 exit 时释放父进程。
- `wait4()` 的 wait status 要保持 Linux 编码：正常退出是 `(exit_code & 0xff) << 8`，信号退出低 7 位存信号。

调度和 CPU：

- `kernel/hal/cpu.hh/.cc` 保存每 CPU 当前进程、调度上下文、中断嵌套状态；当前 QEMU `-smp 1`，但代码仍保留多核抽象。
- `kernel/proc/scheduler.cc` 先扫描最高优先级 nice 值，再选择 RUNNABLE 进程；nice 数值越小优先级越高。
- `yield()` 会把当前 RUNNING 改回 RUNNABLE，再通过 `call_sched()` 和架构 `swtch.S` 切回 CPU 调度上下文。
- RISC-V `Context` 保存 `ra/sp/s0..s11`；LoongArch 保存 `ra/sp/s0..s8/fp`。

## ELF 装载和用户态回归入口

用户态入口：

- RISC-V：`user/app/initcode-rv.cc` 调用 `regression_suite_4d1444_riscv()`。
- LoongArch：`user/app/initcode-la.cc` 调用 `regression_suite_4d1444_loongarch()`。
- 两者结束后调用 `shutdown()`。

系统调用封装：

- `user/syscall_lib/arch/riscv/syscall_arch.hh` 用 `ecall`，系统调用号放 `a7`，参数放 `a0..a5`。
- `user/syscall_lib/arch/loongarch/syscall_arch.hh` 用 `syscall 0`，同样使用 `a7` 和 `a0..a5`。
- `user/syscall_lib/syscall.cc` 提供简化 libc 风格包装。

execve：

- 核心实现在 `ProcessManager::execve()`。
- 会先把相对路径转绝对路径，支持少量 LTP child 程序路径重写。
- 支持 shebang，重写成“解释器 + 脚本路径 + 原 argv[1..]”。
- 先创建新 `ProcessMemoryManager` 和临时新页表，只有装载成功后才替换当前进程旧 mm。
- 加载 ELF `PT_LOAD` 段时按 `p_align` 对齐，LoongArch 16K 对齐段不能退化成 4K。
- 支持 `PT_INTERP` 动态链接器路径映射：
  - RISC-V glibc loader -> `/glibc/lib/ld-linux-riscv64-lp64d.so.1`
  - LoongArch glibc loader -> `/glibc/lib/ld-linux-loongarch-lp64d.so.1`
  - musl loader -> `/musl/lib/libc.so`
- 动态链接器也按自身 `p_align` 装载，入口为 `interp_base + interp_elf.entry`。
- 用户栈当前分配 32 页，压入随机数、env 字符串、argv 字符串、auxv、envp、argv、argc。
- auxv 当前包含 `AT_HWCAP`、`AT_PAGESZ`、`AT_RANDOM`、`AT_PHDR`、`AT_PHENT`、`AT_PHNUM`、`AT_BASE`、`AT_ENTRY`、`AT_NULL`。
- exec 成功后关闭 CLOEXEC fd，清理旧 mm，绑定新 mm，设置 trapframe PC 和 SP。

## 系统调用架构

系统调用号在 `kernel/sys/syscall_defs.hh`，大体沿用 asm-generic Linux syscall 编号。

分发器在 `kernel/sys/syscall_handler.cc`：

- `SyscallHandler::init()` 先把 2048 个槽位设成 `_default_syscall_impl()`，再用 `BIND_SYSCALL(name)` 注册。
- 未实现 syscall 返回 `SYS_ENOSYS`。
- `invoke_syscaller()` 从当前进程 trapframe 的 `a7` 取 syscall number，调用成员函数，并把返回值写回 `a0`。
- `invoke_syscaller()` 会检查 syscall 后用户页表 base 是否看起来被破坏。
- 常用取参助手：`_arg_raw()`、`_arg_int()`、`_arg_long()`、`_arg_addr()`、`_arg_str()`、`_arg_fd()`。

当前 `syscall_handler.cc` 很大，约 1.5 万行，是项目复杂度最高的单文件之一。修改时要先定位已有同类 syscall 的错误码、用户拷贝、fd 检查和 Linux 语义处理方式。

系统调用表注意事项：

- `max_syscall_funcs_num = 2048`，但用户调试 syscall 号 `2021..2025` 超过 2048；调用前要确认分发数组是否能覆盖目标号，避免越界或默认 ENOSYS。
- `SYS_kill = 6` 与 `SYS_lsetxattr = 6` 有历史冲突，`SYS_exec = 55` 与 `SYS_fchown = 55` 也有历史注释；真实 Linux ABI 场景主要走 asm-generic 编号和 `execve=221`。
- `BIND_SYSCALL(memfd_create)` 当前在 init 绑定列表里出现两次；这通常不影响行为，但修改绑定列表时要留心重复绑定和编号冲突。
- 用户态封装在 `user/syscall_lib/syscall.cc` 中，有些包装会把负 errno 转成 `errno`，例如 `setpriority/getpriority`；内核侧仍应返回负 Linux errno。
- `fork()` 用户态包装实际调用 `clone(SIGCHLD, 0, 0, 0, 0)`，因此 clone 语义错误会表现为 fork/basic/LTP 大面积异常。
- 新增 syscall 时需要同步：`kernel/sys/syscall_defs.hh` 编号、`syscall_handler.hh` 声明、`syscall_handler.cc` 实现和 `BIND_SYSCALL`、必要时用户态 `user/syscall_lib` 包装和 LTP 测例开关。

重点 syscall 族：

- 进程：`clone`、`clone3`、`execve`、`wait4`、`exit`、`exit_group`、`setpgid/getpgid/setsid/getsid`。
- 文件：`openat`、`close`、`read/write`、`readv/writev`、`pread/pwrite`、`lseek`、`fcntl`、`ioctl`、`statx/fstatat/fstatfs/statfs`、`renameat2`、`copy_file_range`、`splice`。
- 内存：`brk`、`mmap`、`munmap`、`mremap`、`mprotect`、`madvise`、`msync`、`memfd_create`。
- 同步：`futex`、`set_tid_address`、`set_robust_list/get_robust_list`。
- 信号：`rt_sigaction`、`rt_sigprocmask`、`rt_sigtimedwait`、`rt_sigsuspend`、`rt_sigreturn`、`sigaltstack`、`kill/tkill/tgkill`。
- 时间：`clock_gettime`、`clock_nanosleep`、`nanosleep`、`setitimer/getitimer`、POSIX timer 系列。
- IPC/网络：`shmget/shmat/shmctl/shmdt`、`socket/bind/listen/accept/connect/sendto/recvfrom/...`。

错误码注意：

- 许多 syscall 返回的是负 Linux errno，例如 `-EINVAL`、`-EFAULT`。
- 部分旧代码使用 `syscall::SYS_EINVAL` 这种宏，修改前先看同文件约定。
- 不要随意把 panic 改成静默返回；先区分“内核不变量损坏”和“用户态应得 errno”。

## Trap、中断和用户态返回

RISC-V：

- `kernel/trap/riscv/trap.cc`。
- `trap_manager::inithart()` 设置 `stvec=kernelvec`，打开 SIE 中外部/时钟/软件中断，调用 SBI timer。
- 用户态 syscall cause 为 8，`epc += 4` 后进入 `SyscallHandler`。
- page fault cause 12/13/15 走 `mmap_handler()` 懒分配，失败则给 SIGSEGV。
- timer tick 会更新 `ticks`、唤醒 `&ticks`、检查 POSIX timer 和 interval timer。
- 时间片计数到阈值后 `yield()`。

LoongArch：

- `kernel/trap/loongarch/trap.cc`。
- `inithart()` 设置 `ecfg`、`tcfg`、`eentry`、`tlbrentry`、`merrentry`。
- LoongArch `ESTAT` 混合异常编码和中断 pending，`devintr()` 必须先确认 ecode 为 0 才分发中断，避免同步异常被误判为 timer。
- syscall ecode 为 `0xb`，`era += 4` 后进入 `SyscallHandler`。
- page fault ecode `0x1..0x7`；地址错误 ecode `0x8/0x9` 直接按同步地址错误发 SIGSEGV。
- 对“PTE 已合法但 TLB 残留无效/旧权限”的用户页，会先按页对 `invtlb` 重试，避免误判成 mmap 缺页失败。
- `usertrapret()` 只在 TRAPFRAME PTE 未指向当前线程 trapframe 物理页时重映射并失效 TLB，减少破坏用户态 LL/SC 保留窗口。
- 目前有大量 `debug_pthread_exit_robust_loop()` 和 `debug_entry_static_user_stall()` 调试信息，服务于 LoongArch pthread/LLSC 问题。

## 文件系统和文件对象

块设备和根文件系统：

- `virtio_disk_init()` 初始化主块设备。
- `init_fs_table()` 初始化 `fs_table`，EXT4 和 FAT32 分别注册。
- `filesystem_init()` 在第一个进程上下文中运行：
  - 初始化/扫描 DTB 和 initrd。
  - 注册 initrd 为 RamDisk。
  - 探测主块设备和 ramdisk 是 FAT32、EXT4 还是 unknown。
  - 优先使用主盘 EXT4 作为根文件系统。
  - 若主盘是 FAT32 且 initrd 是 EXT4，则用 initrd 作为根，并把 FAT32 挂到 `/fat32`。
  - 挂载后调用 `dir_init()` 创建/修正 `/dev/misc`、`/tmp`、`/usr`、`/usr/lib`。

ext4：

- ext4 来自 `kernel/fs/lwext4/`。
- VFS 封装在 `kernel/fs/vfs/vfs_ext4_ext.cc` 和 `vfs_ext4_blockdev_ext.cc`。
- `vfs_ext4_init()` 会 `ext4_device_unregister_all()`、`ext4_init_mountpoints()`，并初始化全局可重入 `extlock`。
- `extlock` 是 mount 级串行化锁，允许同一进程递归进入，防止多进程并发 open/stat/readdir/link/unlink 破坏 lwext4 状态。

VFS：

- 新式文件对象是 `fs::file` 抽象类，派生类包括：
  - `normal_file`
  - `directory_file`
  - `device_file`
  - `pipe_file`
  - `socket_file`
  - `virtual_file`
  - `fat32_file`
  - `epoll_file`
- 老式 `struct file` 和 `file_operations` 仍存在于 `kernel/fs/vfs/file.hh`、`file.cc`，部分 ext4 旧路径仍在使用。
- `fs::k_file_table` 是新式 file pool，但很多地方直接 `new normal_file` / `new device_file`。

虚拟文件系统：

- `kernel/fs/vfs/virtual_fs.hh/.cc` 实现树状虚拟路径。
- `fs::k_vfs.dir_init()` 注册大量虚拟文件和设备：
  - `/proc/self/exe`
  - `/proc/meminfo`
  - `/proc/cpuinfo`
  - `/proc/version`
  - `/proc/mounts`
  - `/proc/self/stat`
  - `/proc/self/maps`
  - `/proc/self/pagemap`
  - `/proc/self/status`
  - `/proc/sys/kernel/pid_max`
  - `/proc/sys/kernel/shmmax`、`shmmni`、`shmall`、`tainted`
  - `/etc/ld.so.preload`
  - `/etc/ld.so.cache`
  - `/etc/passwd`
  - `/etc/group`
  - `/dev/loop-control`
  - `/dev/loop0..7`
  - `/dev/block/8:0`
  - `/dev/zero`
  - `/dev/null`
  - `/dev/rtc`、`/dev/rtc0`、`/dev/misc/rtc`

普通文件：

- `normal_file::read/write/lseek` 直接包 lwext4。
- 有文件级 `SleepLock _file_lock`，防止并发读写竞态。
- 写路径处理 sparse write、RLIMIT_FSIZE、immutable/append-only inode flag、memfd seal。
- memfd 有共享状态 `memfd_shared_state`，seal 和 size 是 inode 级共享语义。

文件对象细节：

- 新式 `fs::file` 是抽象基类，核心虚函数是 `read/write/read_ready/write_ready/lseek/read_sub_dir`。
- `file::free_file()` 默认递减 `refcnt`，为 0 时 `delete this`；派生类如果持有额外资源，要保证析构和 close 路径一致。
- `memfd_shared_state` 保存 `refcnt/seals/sealing_allowed/size/backing_path`，用于让通过 `/proc/self/fd/<n>` reopen 出来的对象共享 seal 和大小语义。
- `normal_file` 内嵌 `ext4_file` 和 `ext4_dir` 结构；构造时必须清零，否则虚拟文件/设备文件可能误读旧 flags。
- `directory_file` 负责目录读取；`virtual_file` 通过 `VirtualContentProvider` 生成 `/proc` 等内容；`pipe_file` 包装 `proc::ipc::Pipe`；`socket_file` 目前框架完整但真实网络路径依赖 ONPS 初始化。
- `epoll_file` 是匿名内核文件，路径名类似 `anon_inode:[eventpoll]`，当前主要维护 watch list，不实现真正 epoll wait。
- 老式 `File`/`file_pool` 和新式 `file` 并存；优先判断当前 fd 指针类型，不要想当然迁移。

路径与虚拟文件：

- `vfs_utils.cc` 负责路径规范化、`AT_FDCWD`、相对路径拼接、文件存在检查等常见 VFS 辅助逻辑。
- `VirtualFileSystem` 用树状节点管理虚拟路径，`dir_init()` 注册 `/proc`、`/etc`、`/dev` 下的虚拟文件和设备。
- `is_file_exist()` 会先查虚拟树，再回落到真实 ext4/FAT 路径。
- `/proc/self/*` 与动态 `/proc/<pid>/stat` 有特殊处理；修改 procfs 输出时要兼容 BusyBox/glibc/LTP 对字段数量和格式的要求。

FIFO/pipe：

- `Pipe` 使用动态循环缓冲区，默认 4096 字节，支持 `set_pipe_size()`，上限 16KB。
- `FifoManager` 通过路径映射到 `Pipe`，维护 reader/writer 计数，给命名 FIFO 提供共享后端。
- 管道读写会 sleep/wakeup，涉及非阻塞 flag、读端/写端关闭语义和 `EPIPE`/SIGPIPE 行为。

## 设备、控制台和打印

设备层：

- `DeviceManager` 管理虚拟设备表，预留 stdin/stdout/stderr。
- `register_debug_uart()` 会注册 UART 并把 stdin/stdout/stderr 重定向到 UART。
- `LoopControlDevice::init_loop_control()` 初始化 loop 控制设备。

控制台：

- `dev::kConsole` 是 `Printer` 的输出后端。
- `ConsoleStdin/Stdout/Stderr` 封装标准流。
- RISC-V UART 中断仍通过 SBI console getchar 处理；LoongArch UART 中断当前主要确认并放行。

打印：

- `kernel/libs/printer.cc/.hh` 定义全局 `k_printer`。
- `printf`、`snprintf`、`panic`、`assert` 是宏。
- 支持 `%d/%u/%x/%X/%o/%p/%s/%c/%b`，支持 `l` 和 `z` 长度修饰。
- `disable_printf_flag` 默认 true，颜色宏分 info/warn group 控制。
- `panic` 会设置 panicked 状态，RISC-V 调 SBI shutdown，LoongArch 写关机寄存器。
- 当前很多调试日志用颜色宏包着，是否实际输出取决于 group flag。

设备细节：

- `DeviceManager` 的设备表预留 stdin/stdout/stderr，普通设备从 `DEV_FIRST_NOT_RSV` 往后注册。
- `BlockDevice` 是块设备抽象，virtio disk 和 ramdisk 都走它；`CharDevice` 是字符设备抽象，console、loop-control 等走它。
- `StreamDevice` 可重定向到底层 `CharDevice`，console 标准流就是这一层的常见使用者。
- `RamDisk` 把 initrd 内存区包装成块设备，根文件系统回退路径依赖它。
- `LoopControlDevice` 管理最多 256 个 loop 设备，支持 loop ioctl 相关状态结构，但 `LoopDevice` 本身没有继承 `BlockDevice`，使用前要确认调用路径。
- RISC-V virtio 块设备主路径在 `kernel/fs/drivers/riscv/virtio2.hh` 和 `virtio_disk2.cc`；LoongArch 同时有 `devs/loongarch` 和 `fs/drivers/loongarch` 两套痕迹，当前 main 里调用的是 `virtio_probe()` + `virtio_disk_init()`。

Printer 细节：

- `Printer::init()` 初始化 `dev::kConsole` 并将 `_console` 指向它，早于大部分日志。
- 普通 `print()` 如果 `disable_printf_flag` 为 true 会直接返回；`panic` 不受这个开关影响。
- `warn_group_flag` 控制 Red/Yellow/Orange/Pink 等 warn 类颜色宏，`info_group_flag` 控制 Green/Blue/Cyan/White/Magenta 等 info 类颜色宏。
- `snprint()` 与 `print()` 共用格式化逻辑，但要注意 freestanding 环境中不能引入 libc printf。
- `panic_impl()` 输出后设置 `_panicked`，RISC-V 走 `sbi_shutdown()`，LoongArch 写 `0x8000000000000000 | 0x100E001C` 关机。
- 修改 printer 时必须保持锁逻辑简单，panic 路径不能依赖会 sleep 或分配内存的能力。

## 信号、futex、共享内存和时间

信号：

- `kernel/proc/signal.hh/.cc` 实现 Linux 信号号、`rt_sigaction` ABI、sigmask、altstack、同步信号、信号帧和 `sigreturn`。
- LoongArch 的用户 `sigset_t` 是 128 字节，信号上下文布局和 RISC-V 不同。
- `handle_signal()` 在返回用户态前处理异步信号，`handle_sync_signal()` 优先处理同步信号。
- `kernel_sigaction_abi` 表示 syscall 入口看到的 Linux 内核 ABI 布局：handler、flags、restorer、mask；不要和 libc 暴露的 `struct sigaction` 混淆。
- RISC-V 的 `usercontext` 中机器上下文包含简化 `generalregs/floatregs`；LoongArch 的 `usercontext` 包含 128 字节 `user_sigset` 和 `machinecontext{pc, gregs[32], flags}`。
- `sigaltstack` 支持 `SS_ONSTACK/SS_DISABLE/SS_AUTODISARM`，LoongArch 的 `MINSIGSTKSZ/SIGSTKSZ` 比 RISC-V 大。
- 默认信号行为通过 `get_default_signal_action()` 判断 terminate/coredump；同步异常如 SIGSEGV 应优先走同步信号处理，避免普通 pending signal 顺序覆盖异常语义。

futex：

- `kernel/proc/futex.cc`。
- `futex_wait()` 先 copy_in 检查用户值；不匹配返回 EAGAIN。
- 带 timeout 的 futex 睡在 `tmm::k_tm.get_tick_wait_channel()` 上，每 tick 被唤醒后重新检查 deadline。
- 被信号唤醒应返回 EINTR，不要误判为正常 wake。
- `futex_cleanup_robust_list()` 处理 robust futex owner died 和 waiters wake。
- robust futex ABI 结构在 `kernel/proc/futex.hh`，包括 `robust_list_head::futex_offset` 和 `list_op_pending`。
- `FUTEX_OWNER_DIED`、`FUTEX_WAITERS`、`FUTEX_TID_MASK` 用于 pthread mutex 退出清理；LoongArch pthread 问题常会落在这里和 LL/SC/TLB 的交界处。

共享内存：

- `kernel/shm/shm_manager.hh/.cc`。
- PMM 给 shm 单独切出一段区域。
- `ShmManager` 管理 `shm_segment`，记录 key、size、real_size、权限、attached_addrs、物理地址、时间、owner、nattch、seq。
- attach 记录按 `(tid, addr)` 管理，避免线程/进程地址混淆。
- fork 时可以复制父 tid 的 attach 记录到子 tid。
- 退出或创建失败时要走 `detach_all_for_process()` 清理。
- `create_seg()`/`find_seg_by_key()`/`delete_seg()` 管理 SysV shmid 和 key；`shmctl()` 支持 `IPC_STAT/IPC_SET/IPC_RMID` 等语义。
- `attach_seg()` 需要处理 `SHM_RND`、`SHM_RDONLY`、地址冲突、权限检查和页表映射。
- `find_shared_memory_segment()` 用于判断地址是否属于共享段；mmap(MAP_SHARED) 失败清理时要区分“本次新建段”和“复用历史段”。

时间：

- `TimerManager` 管理 sleep、clock_gettime、ticks、tick wait channel。
- trap 的 timer tick 推进全局 tick，唤醒睡眠通道，检查 POSIX timer 和 per-process interval timer。
- RISC-V 使用 SBI timer；LoongArch 使用 CSR timer，`tcfg` 必须与 `tmm::cycles_per_tick()` 保持一致。
- `TimerManager::get_time_val()` 支撑 `gettimeofday`；`clock_gettime()` 支持多种 POSIX clock id。
- `proc::Pcb` 里保存 `_utime/_stime/_cutime/_cstime/_start_time/_itimer[]`；trap 进入/退出用户态会更新用户态/内核态时间。
- POSIX timer 相关代码在 `kernel/proc/posix_timers.cc/.hh`，interval timer 检查在 trap 的 timer tick 中触发。
- `clock_nanosleep`、`nanosleep`、futex timeout 都依赖 tick 唤醒，调试超时类 LTP 时要同时看 trap timer 和 `TimerManager`。

## 网络

- `kernel/net/f7ly_network.cc` 集成 VirtIO Net 和 ONPS。
- `init_network_stack()` 会加载 ONPS，再初始化 virtio net adapter。
- 当前 RISC-V `main.cc` 中 `net::init_network_stack()` 是注释状态。
- `socket_file` 已实现一批 TCP/UDP/UNIX socket 框架，但 UNIX domain socket 和真实网络路径有不少 TODO。
- LTP 排名里 `splice07` 注释提示“测这个记得开网络(init_network_stack)”，因此网络相关用例开启前要确认 QEMU `-netdev user` 和 ONPS 状态。
- ONPS 代码在 `kernel/net/onpstack/`，包含 ethernet、ARP、IP、TCP、UDP、route、netif、buddy/buf_list 和 OS adapter。
- VirtIO Net 适配层在 `kernel/net/drivers/virtio_net.*` 和 `virtio_net_adapter.*`。
- `socket_file` 保存 socket state、family/type/protocol、ONPS socket 句柄、本地/远端地址、listen backlog、收发缓冲和 blocking/reuse_addr 标志。
- socket syscall 在 `syscall_handler.cc`，文件对象在 `kernel/fs/vfs/file/socket_file.cc`；网络栈未初始化时应优先确认返回 errno 是否符合 Linux 语义，而不是只看 socket_file 层。

## 用户态回归套件

`user/user_lib/user_test.cc` 是回归入口：

- `run_test()` fork 子进程，子进程 `setpgid(0,0)` 后 exec，父进程 waitpid 并解码 wait status。
- 每个测例独立进程组很重要，避免 LTP 超时清理 `kill(0, SIGKILL)` 把 init 一起杀掉。
- `init_env()` 会进入 `/musl/` 或 `/glibc/` 并执行 `busybox --install /bin`。
- `basic_test()` 跑基础 syscall 测试。
- `ltp_test(bool is_musl)` 跑 `ltp_testcases[]` 中打开的 LTP 用例，按架构布尔开关跳过。
- `regression_suite_4d1444_riscv()` 当前顺序大致是 musl basic、glibc basic、busybox、musl LTP、glibc LTP、musl libc-test、glibc lua、libcbench。
- `regression_suite_4d1444_loongarch()` 当前也会跑 musl/glibc basic、musl/glibc LTP、busybox、libc-test、lua、libcbench。

调试测例时的关键背景：

- RISC-V 和 LoongArch 当前目标都是跑测例，而不是进入交互 shell。
- 测例大类包括 basic、busybox、ltp、lua、libcbench 等。
- 除 LTP 以外，basic/busybox/lua/libcbench 等测例源码来自 `ref/testsuits-for-oskernel`。
- 如果 `ref/testsuits-for-oskernel` 不存在，可以按需求从 GitHub 下载对应仓库，统一放在 `ref/` 目录下；不要把外部参考源码散放到项目根目录。
- 当前自写用户态入口在 `user/app/initcode-rv.cc` 和 `user/app/initcode-la.cc`，它们选择调用对应架构的 regression suite。
- `user/user_lib/user_test.cc` 是各个单元测试的调度代码，可以通过修改 regression suite、`basic_test()`、`busybox_test()`、`ltp_test()`、`lua_test()`、`libcbench_test()` 等入口，只打开目标小测例，避免每次跑完整长测例。
- 定位单个失败时，优先缩小到一个架构、一个 libc 目录、一个测试大类、一个小测例；单条测例最多跑 5 分钟，确认现象后再扩大范围回归。

`ltp_testcases[]` 很长，且注释明确说明：新开未跑过的 LTP 测例时优先按 `tools/ltp/judge/ltp_rank.txt` 的 total count 从高到低推进。

用户态封装注意：

- `user/syscall_lib/arch/riscv/syscall_arch.hh` 和 `arch/loongarch/syscall_arch.hh` 都把 syscall 号放在 `a7`，参数放在 `a0..a5`。
- `user/syscall_lib/syscall_base.hh` 是低层 syscall 触发封装，`syscall.cc` 是类 libc 的便捷函数。
- `user/deps/user.hh` 汇总用户态可见声明；如果新增用户态测试入口或封装，通常要同步这里。
- `run_test()` 中子进程先 `setpgid(0,0)` 再 exec，这是为了隔离 LTP 超时清理信号；不要随意删除。
- `decode_wait_status()` 按 Linux wait status 解码；如果内核 wait status 改错，用户态会显示 exit/signal 判定异常。
- `init_env()` 会进入 musl/glibc 根目录并执行 `busybox --install /bin`，因此 `/bin`、busybox、动态库路径和 cwd 都会影响后续大批测试。
- musl LTP 环境变量使用 `LD_LIBRARY_PATH=/musl/lib`，glibc 使用 `/glibc/lib`；不要混用，否则动态程序会以很迷惑的方式失败。

## LTP 分析工具

- `tools/ltp/judge/judge_ltp_musl.py`：从 QEMU 输出中解析 `RUN LTP CASE`、`Summary:`、`END/FAIL LTP CASE`，输出 JSON。
- `tools/ltp/judge/analyze_output.sh <output_file>`：
  - 调用 `judge_ltp_musl.py`。
  - 生成 `analysis_results.txt` 和 `analysis_rank.txt`。
  - 会用 `python3`、`grep`、`awk`、`bc`、`mktemp`。
  - 注意它会在当前目录写分析文件，运行前确认目录。
- `tools/ltp/judge/analyze_ltp_results.py`：分析 `ltp_rank.txt`，但它有交互输入，不适合无人值守直接跑。
- `tools/ltp/judge/ltp_rank.txt`：按收益排序的 LTP 参考表，第四列常有人工备注，例如 pass、loopdev、no、可以修等。
- `tools/ltp/scoreboard/parse_ltp_scoreboard.py` 和 `generate_cpp_array.py` 用于把 scoreboard 转成 C++ 数组。

运行 Python 分析脚本前仍需遵守项目 Python 约定：创建/激活 venv，并确认解释器路径。

分析工具细节：

- `judge_ltp_musl.py` 只认 `RUN LTP CASE <name>` 后的 `Summary:` 统计，以及 `END LTP CASE` 或 `FAIL LTP CASE` 结束行。
- 它输出的 JSON 字段是 `name/pass/all/score`，其中 `pass` 当前等于 summary 的 passed 数，不直接看进程退出码。
- `analyze_output.sh` 会在当前工作目录写 `analysis_results.txt` 和 `analysis_rank.txt`，还会创建临时排序文件；运行前要确认不会覆盖手工分析结果。
- `analyze_output.sh` 内部直接调用 `python3 judge_ltp_musl.py`，所以应在 `tools/ltp/judge/` 目录内运行，或手动调整路径。
- `analyze_ltp_results.py` 有交互输入，会询问是否显示详细分析、是否保存结果；自动化场景不要直接跑。
- `ltp_rank.txt` 的第四列是人工备注，不是机器可解析的稳定字段；推进测例时优先按第二/三列收益和备注综合判断。

## LoongArch 当前重点风险

最近提交和未跟踪上下文都显示当前 LoongArch 工作重点集中在 pthread/LLSC/线程退出。

当前代码中值得注意的点：

- `tools/patch_loongarch_libctest_llsc.sh` 会原地修补 `images/sdcard-la.img` 中 `/musl/entry-static.exe` 和 `/musl/lib/libc.so` 的部分 LL/SC 指令序列。
- 该脚本幂等，使用 `debugfs` dump/patch/write 回镜像。
- `Makefile` 的 `run-loongarch` 和 `debug-loongarch` 会先跑这个 patcher。
- LoongArch `trap.cc` 保留了大量 pthread exit / entry-static stall 调试 panic。
- `llsc_exec_probe.cc` 是用于排除普通 signal、PC rewrite、单线程 LL/SC 问题的对照探针，但当前 `initcode-la.cc` 已回到完整回归入口。
- 当前最近提交信息“la回退到pthread不能跑的位置”说明 LoongArch 仍处于不稳定/定位阶段；修改 LA trap、页表、TLB、线程切换、futex、signal 前务必先保留可复现日志。

排查 LoongArch pthread/LLSC 时优先关注：

- `kernel/trap/loongarch/trap.cc`
- `kernel/mem/loongarch/pagetable.*`
- `kernel/trap/loongarch/tlbrefill.S`
- `kernel/trap/loongarch/uservec.S`
- `kernel/proc/loongarch/swtch.S`
- `kernel/proc/futex.cc`
- `kernel/proc/signal.cc`
- `kernel/proc/proc_manager.cc` 中 clone/exit/wait/clear_tid 相关逻辑

LoongArch trap/TLB 关键细节：

- `loongarch_exception_code()` 从 `ESTAT[21:16]` 取一级异常码，`loongarch_exception_subcode()` 从 `ESTAT[30:22]` 取二级码；不要直接把整个 ESTAT 当 ecode。
- page fault ecode 为 `0x1..0x7`，地址错误 `0x8/0x9` 应走 SIGSEGV，不应进入 mmap 懒分配。
- `loongarch_can_retry_present_user_fault()` 用于“页表 PTE 已经 present 且权限正确，但 TLB 残留无效项”的重试路径。
- `loongarch_invalidate_user_tlb_page()` 按相邻两页对齐失效，因为 LoongArch 普通 TLB 项覆盖 pair page。
- `loongarch_ack_timer_interrupt()` 直接写 `CSR_TICLR_CLR`，不要读改写 TICLR。
- `probe_loongarch_tlb()`、`count_pa_as_leaf_mapping()`、`count_pa_as_pagetable_page()` 等函数主要用于当前 pthread/LLSC 调试，不是普通热路径抽象。
- `debug_pthread_exit_robust_loop()` 盯 `entry-static.exe` 的固定 PC 和 robust mutex 状态，达到阈值会输出/触发调试信息。
- `debug_entry_static_user_stall()` 用于定位 entry-static 用户态停滞；这些函数留有大量硬编码地址，换镜像或二进制后要重新确认。
- `usertrapret()` 的 trapframe 动态映射是 LoongArch 多线程的核心保护：只有 PTE 指向的物理页不是当前线程 trapframe 时才 remap + invtlb。

## 修改代码时的高风险区域

- `kernel/sys/syscall_handler.cc`：超大文件，容易引入返回值/errno/用户拷贝/引用计数不一致。
- `kernel/proc/proc_manager.cc`：进程生命周期、fork/clone/exec/exit/wait 互相耦合。
- `kernel/proc/process_memory_manager.cc`：内存释放、VMA split、共享 mm 引用计数容易双重释放或泄漏。
- `kernel/trap/loongarch/trap.cc`：TLB、trapframe、LL/SC、同步异常和中断分发非常敏感。
- `kernel/fs/vfs/vfs_ext4_ext.cc`：lwext4 需要串行化，不能绕过 `extlock` 破坏并发保护。
- `kernel/libs/printer.cc`：早期启动和 panic 都依赖它，格式化函数必须 freestanding 安全。
- `Makefile`：构建规则同时影响双架构、initcode、EASTL、镜像补丁，不要只测一边就改公共规则。

代码风格和实现习惯：

- 内核代码大量使用全局单例，如 `mem::k_pmm`、`mem::k_vmm`、`mem::k_hmm`、`proc::k_pm`、`proc::k_scheduler`、`syscall::k_syscall_handler`、`tmm::k_tm`、`shm::k_smm`、`fs::k_vfs`、`dev::k_devm`。
- C++ 代码运行在 freestanding 环境，不能默认使用完整 libc、异常、RTTI、全局析构能力。
- EASTL 是主要容器来源，`eastl::string/vector/unordered_map/unique_ptr/atomic` 在内核中广泛使用。
- 代码中有不少历史注释、玩笑命名和 TODO，判断真实行为要以当前调用链和最近 commit 为准。
- 需要新增注释时优先写清楚“为什么这么做”和“该逻辑保护什么不变量”，不要只解释语法。
- 不要为了短期兼容保留新旧双入口；本项目约定需求升级时统一契约、删除旧逻辑。
- 镜像、QEMU 输出和二进制一般是生成物；除非用户明确要求，不要纳入 commit。

## 常见开发判断

- 需要理解历史决策时，优先看最近 commit message，再看 `docs/dev-notes/` 和 README。
- 运行内核排查问题时，完整测试使用 40 分钟 timeout，单条测例最多使用 5 分钟 timeout，日志保存到 `output_*.txt`；聊天里只摘关键行和结论。
- debug 时不要每次都跑完整测试；修哪个测例就通过 `user/user_lib/user_test.cc` 或 `user/app/initcode-*.cc` 只打开哪个测例。
- 代码里很多 TODO/历史注释已经过时，不能只看注释判断功能是否可用。
- 同一个概念可能有新旧两套实现，例如老 `struct file` 与新 `fs::file`，修改时要确认当前调用链走哪套。
- `README.md` 偏项目介绍，运行命令有参考价值，但细节以 `Makefile` 为准。
- `docs/archive/` 和 `docs/dev-notes/` 偏历史归档，不要未经核对就当成当前实现。
- 镜像、输出日志、内核二进制默认是生成物；除非用户明确要求，不要纳入提交。

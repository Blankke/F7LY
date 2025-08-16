= 系统调用

== 系统调用概述

系统调用是用户程序与操作系统内核之间的接口，允许用户程序请求内核提供服务，如文件操作、进程管理、内存分配等。F7LY内核实现了符合 POSIX 标准的系统调用接口，并提供了与 Linux 内核兼容的系统调用列表。

当用户态程序需要申请内核资源或执行特权操作时，会通过系统调用进入内核。这一过程由硬件触发用户态到内核态的陷入（`usertrap`），在陷入点内核会根据异常码进行判断，并通过 `syscall_handler` 包装逻辑进入具体的系统调用处理流程。

在 F7LY 中，系统调用的管理由专用的 `SyscallHandler` 类完成，并在内核启动的 `main` 函数中统一完成系统调用表的初始化绑定。其核心结构示例如下：

```cpp
#define BIND_SYSCALL(sysname)                                       \
    _syscall_funcs[SYS_##sysname] = &SyscallHandler::sys_##sysname; \
    _syscall_name[SYS_##sysname] = #sysname;

class SyscallHandler
{
    using SyscallFunc = uint64 (SyscallHandler::*)(); 
private:
    SyscallFunc _syscall_funcs[max_syscall_funcs_num]; 
    const char *_syscall_name[max_syscall_funcs_num]; 
    uint64_t _default_syscall_impl();  
    ......
public:
    void init();             // 使用构造函数进行init
    void invoke_syscaller(); // 调用系统调用
private:
    int _fetch_addr(uint64 addr, uint64 &out_data);
    int _fetch_str(uint64 addr, eastl::string &buf, uint64 max);
    uint64 _arg_raw(int arg_n);
    int _arg_int(int arg_n, int &out_int);
    int _arg_long(int arg_n, long &out_int);
    int _arg_addr(int arg_n, uint64 &out_addr);
    int _arg_str(int arg_n, eastl::string &buf, int max);
    int _arg_fd(int arg_n, int *out_fd, fs::file **out_f);
}
extern SyscallHandler k_syscall_handler;
```

== 系统调用流程

=== 系统调用执行流程

F7LY 内核的系统调用执行流程可以分为以下几个关键步骤：

+ *用户态触发*：用户程序通过 `ecall` 指令（RISC-V）或 `syscall` 指令（LoongArch）触发系统调用，产生异常并陷入内核态。

+ *异常处理*：硬件自动保存用户态上下文，并跳转到内核的异常处理入口 `usertrap`。

+ *系统调用分发*：内核根据异常码判断为系统调用异常，调用 `syscall_handler` 进行系统调用分发。

+ *参数获取*：通过寄存器获取系统调用号和参数，调用 `SyscallHandler::invoke_syscaller` 方法。

+ *函数查表执行*：根据系统调用号在 `_syscall_funcs` 数组中查找对应的处理函数并执行。

+ *返回用户态*：将执行结果放入返回寄存器，恢复用户态上下文并返回。

#figure(
  image("fig/系统调用.png", width: 70%),
  caption: [系统调用示意图],
) <fig:syscall>

=== 参数获取机制

F7LY 内核提供了一套完整的参数获取机制，通过 `SyscallHandler` 类的私有方法实现：

```cpp
// 获取原始参数（从寄存器直接读取）
uint64 SyscallHandler::_arg_raw(int arg_n) {
    // 从当前进程的 trapframe 中获取第 arg_n 个参数
    // RISC-V: a0-a5 寄存器存储前6个参数
    // LoongArch: $a0-$a5 寄存器存储前6个参数
    return current_proc()->trapframe->regs[10 + arg_n]; // a0 = x10
}

// 获取整型参数
int SyscallHandler::_arg_int(int arg_n, int &out_int) {
    out_int = (int)_arg_raw(arg_n);
    return 0;
}

// 获取地址参数并验证
int SyscallHandler::_arg_addr(int arg_n, uint64 &out_addr) {
    out_addr = _arg_raw(arg_n);
    // 验证地址是否在用户空间范围内
    if (out_addr >= MAXVA) {
        return -1; // 地址无效
    }
    return 0;
}

// 获取字符串参数
int SyscallHandler::_arg_str(int arg_n, eastl::string &buf, int max) {
    uint64 addr;
    if (_arg_addr(arg_n, addr) < 0) {
        return -1;
    }
    return _fetch_str(addr, buf, max);
}

// 从用户空间获取字符串
int SyscallHandler::_fetch_str(uint64 addr, eastl::string &buf, uint64 max) {
    buf.clear();
    for (uint64 i = 0; i < max; i++) {
        char c;
        if (copyinstr(current_proc()->pagetable, (char*)&c, addr + i, 1) < 0) {
            return -1; // 页面错误
        }
        if (c == '\0') {
            break;
        }
        buf.push_back(c);
    }
    return 0;
}
```

=== 系统调用表初始化

系统调用表的初始化在内核启动时的 `main` 函数中完成，通过 `SyscallHandler::init()` 方法实现：

```cpp
void SyscallHandler::init() {
    // 初始化所有系统调用为默认实现
    for (int i = 0; i < max_syscall_funcs_num; i++) {
        _syscall_funcs[i] = &SyscallHandler::_default_syscall_impl;
        _syscall_name[i] = "unknown";
    }
    
    // 使用宏绑定具体的系统调用实现
    BIND_SYSCALL(read);          // sys_read
    BIND_SYSCALL(write);         // sys_write
    BIND_SYSCALL(open);          // sys_open
    // ... 更多系统调用绑定
}

// 默认系统调用实现
uint64_t SyscallHandler::_default_syscall_impl() {
    uint64 syscall_num = _arg_raw(-1); 
    kprintf("[SYSCALL] Unimplemented syscall: %d\n", syscall_num);
    return -ENOSYS; // 返回"功能未实现"错误码
}
```

=== 系统调用分发器

系统调用的核心分发逻辑由 `invoke_syscaller` 方法实现：

```cpp
void SyscallHandler::invoke_syscaller() {
    uint64 syscall_num = _arg_raw(-1);     
    // 边界检查
    if (syscall_num >= max_syscall_funcs_num) {
        current_proc()->trapframe->regs[10] = _default_syscall_impl();
        return;
    }
    
    // 获取对应的系统调用函数指针
    SyscallFunc func = _syscall_funcs[syscall_num];
    
    // 执行系统调用并将结果存储到 a0 寄存器
    uint64 result = (this->*func)();
    current_proc()->trapframe->regs[10] = result; 
    // 记录系统调用执行信息（调试模式）
    #ifdef DEBUG_SYSCALL
    kprintf("[SYSCALL] %s(%d) -> %ld\n", 
            _syscall_name[syscall_num], syscall_num, result);
    #endif
}
```

这种设计的优势在于：
- *统一性*：所有系统调用都通过统一的接口进行分发
- *可扩展性*：通过简单的宏定义即可添加新的系统调用
- *容错性*：未实现的系统调用会调用默认实现，避免内核崩溃
- *调试友好*：提供系统调用名称和编号的映射，便于调试

== 系统调用实现

目前F7LY内核已经实现并验证了超过210个系统调用，涵盖了进程管理、内存管理、文件操作、网络通信、信号处理等核心功能。每个系统调用都通过 `SyscallHandler` 类的成员函数实现，并在内核启动时进行绑定。本节按照功能模块分类，按常用程度介绍主要系统调用的实现。

=== 进程生命周期管理

进程生命周期管理是操作系统的核心功能之一，F7LY实现了完整的进程创建、执行、等待和终止机制。

==== 进程创建与执行

- `sys_fork()`：创建新进程，完全复制父进程的地址空间、文件描述符表和进程上下文。新进程获得新的PID，与父进程并发执行。
- `sys_clone()`：Linux风格的进程/线程创建接口，支持更细粒度的资源共享控制。可以指定哪些资源在父子进程间共享（如内存空间、文件描述符、信号处理等）。
- `sys_clone3()`：clone的扩展版本，提供更多的创建选项和参数。
- `sys_execve()`：加载并执行新程序，替换当前进程映像。支持参数传递（argv）和环境变量设置（envp），通过ELF加载器实现程序加载。
- `sys_exec()`：简化版的程序执行接口。

==== 进程等待与终止

- `sys_wait4()`：等待子进程状态改变，获取退出状态。支持阻塞等待和非阻塞查询，可指定等待特定PID的子进程。
- `sys_wait()`：简化版的等待接口，等待任意子进程终止。
- `sys_waitid()`：更灵活的等待接口，支持等待多种进程状态改变事件。
- `sys_exit()`：正常终止当前进程，设置退出码并释放进程资源。
- `sys_exit_group()`：终止整个线程组（进程组），用于多线程程序的统一退出。

==== 进程标识与信息获取

- `sys_getpid()`：获取当前进程ID。
- `sys_getppid()`：获取父进程ID。
- `sys_gettid()`：获取当前线程ID。
- `sys_getuid()`/`sys_geteuid()`：获取用户ID和有效用户ID。
- `sys_getgid()`/`sys_getegid()`：获取组ID和有效组ID。
- `sys_getpgid()`/`sys_setpgid()`：进程组管理。
- `sys_setsid()`：创建新会话并设置进程组长。

=== 内存管理

内存管理系统调用提供了虚拟内存映射、动态内存分配和内存保护等功能。

==== 内存映射与解映射

- `sys_mmap()`：内存映射核心接口，支持文件映射、匿名映射、共享内存映射。可指定映射地址、保护属性（读/写/执行）和映射标志。
- `sys_munmap()`：解除内存映射，释放指定地址范围的虚拟内存。
- `sys_mremap()`：重新映射内存区域，支持扩展或收缩映射大小。
- `sys_mprotect()`：修改内存页的保护属性，实现内存访问控制。
- `sys_madvise()`：向内核提供内存使用建议，优化内存管理策略。
- `sys_msync()`：同步内存映射文件的修改到存储设备。

==== 堆内存管理

- `sys_brk()`：调整数据段大小，用于堆内存的扩展和收缩。是malloc库的底层实现基础。

==== 共享内存（System V IPC）

- `sys_shmget()`：创建或获取共享内存段。
- `sys_shmat()`：将共享内存段连接到进程地址空间。
- `sys_shmdt()`：从进程地址空间分离共享内存段。
- `sys_shmctl()`：共享内存控制操作（获取状态、删除等）。

=== 文件系统操作

文件系统操作是系统调用中最丰富的类别，提供了完整的文件和目录操作接口。

==== 文件打开与关闭

- `sys_openat()`：相对目录打开文件，支持各种打开标志（只读、只写、读写、创建、追加等）。
- `sys_openat2()`：扩展版本，提供更多的打开选项和安全控制。
- `sys_close()`：关闭文件描述符，释放相关资源。
- `sys_close_range()`：批量关闭一个范围内的文件描述符。

==== 文件读写操作

- `sys_read()`：从文件描述符读取数据到缓冲区。
- `sys_write()`：将缓冲区数据写入文件描述符。
- `sys_readv()`/`sys_writev()`：矢量读写，支持多个缓冲区的批量读写。
- `sys_pread64()`/`sys_pwrite64()`：定位读写，在指定偏移位置进行读写操作，不改变文件位置指针。
- `sys_preadv()`/`sys_pwritev()`：定位矢量读写。
- `sys_lseek()`：设置文件位置指针。
- `sys_sendfile()`：在两个文件描述符间高效传输数据，零拷贝实现。

==== 文件状态与属性

- `sys_fstat()`：获取文件描述符对应文件的状态信息。
- `sys_fstatat()`：获取相对路径文件的状态信息。
- `sys_statx()`：扩展的文件状态查询接口，提供更丰富的文件信息。
- `sys_faccessat()`/`sys_faccessat2()`：检查文件访问权限。
- `sys_readlinkat()`：读取符号链接的目标路径。

==== 目录操作

- `sys_mkdirat()`：创建目录。
- `sys_getdents64()`：读取目录项，用于ls命令的实现。
- `sys_getcwd()`：获取当前工作目录。
- `sys_chdir()`：改变当前工作目录。
- `sys_fchdir()`：通过文件描述符改变工作目录。

==== 文件系统管理

- `sys_mount()`：挂载文件系统。
- `sys_umount2()`：卸载文件系统。
- `sys_statfs()`/`sys_fstatfs()`：获取文件系统统计信息。
- `sys_sync()`/`sys_fsync()`/`sys_fdatasync()`：文件系统同步操作。

=== 信号处理

F7LY实现了完整的POSIX信号机制，支持信号的发送、捕获和处理。

==== 信号发送

- `sys_kill()`：向进程发送信号。
- `sys_tkill()`：向特定线程发送信号。
- `sys_tgkill()`：向特定线程组中的线程发送信号。
- `sys_rt_sigqueueinfo()`：发送带数据的实时信号。

==== 信号处理设置

- `sys_rt_sigaction()`：设置信号处理函数。
- `sys_rt_sigprocmask()`：设置信号屏蔽字。
- `sys_rt_sigpending()`：查询待处理信号。
- `sys_rt_sigsuspend()`：临时设置信号屏蔽字并等待信号。
- `sys_rt_sigtimedwait()`：等待指定信号，支持超时。
- `sys_rt_sigreturn()`：从信号处理函数返回。
- `sys_sigaltstack()`：设置备用信号栈。

=== 网络通信（Socket）

F7LY提供了完整的BSD Socket API实现，支持TCP/UDP网络通信。

==== Socket创建与管理

- `sys_socket()`：创建socket。
- `sys_socketpair()`：创建socket对，用于进程间通信。

==== 网络连接

- `sys_bind()`：绑定socket到本地地址。
- `sys_listen()`：监听连接请求。
- `sys_accept()`/`sys_accept4()`：接受连接请求。
- `sys_connect()`：主动建立连接。

==== 数据传输

- `sys_sendto()`/`sys_recvfrom()`：UDP数据传输。
- `sys_sendmsg()`/`sys_recvmsg()`：复杂消息传输。
- `sys_getsockname()`/`sys_getpeername()`：获取socket地址信息。
- `sys_setsockopt()`/`sys_getsockopt()`：socket选项设置和查询。

=== 时间管理

==== 时间获取与设置

- `sys_gettimeofday()`：获取当前时间。
- `sys_clock_gettime()`/`sys_clock_settime()`：高精度时间操作。
- `sys_clock_getres()`：获取时钟精度。
- `sys_times()`：获取进程时间统计。
- `sys_uptime()`：获取系统运行时间。

==== 定时器与睡眠

- `sys_nanosleep()`/`sys_clock_nanosleep()`：高精度睡眠。
- `sys_timer_create()`/`sys_timer_settime()`/`sys_timer_delete()`：POSIX定时器。
- `sys_setitimer()`：间隔定时器。

=== 系统信息与控制

- `sys_uname()`：获取系统信息。
- `sys_sysinfo()`：获取系统运行状态。
- `sys_getrusage()`：获取资源使用统计。
- `sys_prctl()`：进程控制操作。
- `sys_sched_yield()`：主动让出CPU。
- `sys_sched_setscheduler()`/`sys_sched_getscheduler()`：调度策略设置。
- `sys_sched_setaffinity()`/`sys_sched_getaffinity()`：CPU亲和性设置。

=== 其他重要系统调用

==== 管道与重定向

- `sys_pipe2()`：创建管道。
- `sys_dup()`/`sys_dup3()`：复制文件描述符。

==== 同步原语

- `sys_futex()`：快速用户空间互斥锁。

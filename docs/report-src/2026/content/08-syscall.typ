= 第八章　系统调用

== 8.1　系统调用概述

系统调用是用户程序与操作系统内核之间的接口，允许用户程序请求内核提供服务，如文件操作、进程管理、内存分配等。F7LY 实现了与 Linux ABI 兼容的系统调用接口，当前已绑定超过 240 个系统调用（完整列表见附录 B），覆盖进程管理、内存管理、文件系统、信号、时间与定时器、Futex、事件通知、网络、权限、系统信息等十个功能类别。

当用户态程序需要申请内核资源或执行特权操作时，通过 `ecall` 指令触发硬件陷入，`usertrap()` 根据异常原因码进行判断，并通过 `SyscallHandler` 分发到对应的处理函数。

系统调用的管理由 `SyscallHandler` 类完成，在内核启动的 `main()` 函数中统一完成系统调用表的初始化绑定。其核心结构示例如下：

```cpp

class SyscallHandler
{
    // 使用了这个类的成员函数指针
    using SyscallFunc = uint64 (SyscallHandler::*)(); // 定义一个函数指针类型 SyscallFunc
private:
    SyscallFunc _syscall_funcs[max_syscall_funcs_num]; // 存储系统调用函数的指针数组
    const char *_syscall_name[max_syscall_funcs_num];  // 存储系统调用名称的指针数组
    uint64_t _default_syscall_impl();                  // 默认的系统调用实现
public:
    void init();             // 使用构造函数进行init
    void invoke_syscaller(); // 调用系统调用
    SyscallHandler()
    {
    }

private:
    int _fetch_addr(uint64 addr, uint64 &out_data);
    int _fetch_str(uint64 addr, eastl::string &buf, uint64 max);
    uint64 _arg_raw(int arg_n);

    int _arg_int(int arg_n, int &out_int);
    int _arg_long(int arg_n, long &out_int);
    int _arg_addr(int arg_n, uint64 &out_addr);
    int _arg_str(int arg_n, eastl::string &buf, int max);
    int _arg_fd(int arg_n, int *out_fd, fs::file **out_f);
    int argfd(int n, int *pfd, struct file **pf);
    bool is_bad_addr(uint64 addr);

};

extern SyscallHandler k_syscall_handler;
```


== 8.2　系统调用流程

=== 系统调用执行流程

1. 用户态陷入：用户程序将调用号和参数放入约定寄存器后，执行 `ecall` 指令。硬件将 CPU 从用户模式切换到内核模式，保存当前寄存器现场，跳转到内核的陷阱处理入口。
2. 陷阱分发：内核陷阱处理函数根据硬件传递的异常原因码，判断这是一次系统调用请求，然后交给系统调用分发器处理。
3. 查表与执行：分发器根据调用号在系统调用表中查找对应的处理函数，从用户寄存器中取出参数传入，执行具体的服务逻辑，最后将返回值写回寄存器。若调用号未绑定处理函数，则返回 `-ENOSYS`，表示该调用未实现。
4. 返回用户态：内核检查是否有待投递的信号，随后恢复用户程序的寄存器现场，切换回用户页表，通过硬件返回指令回到用户态继续执行。

===  参数获取机制

F7LY 内核提供了一套完整的参数获取机制，通过 `SyscallHandler` 类的私有方法实现。最底层从寄存器中直接读取六个参数的原始值，之上分层提供整数、地址、字符串和文件描述符的类型化提取。其中地址和字符串参数不直接解引用用户态指针，而是经由页表校验和 `copy_in`/`copy_out` 完成数据搬运，防止非法指针导致内核崩溃。文件描述符提取则额外包含悬空指针检测，发现失效条目时自动清理。

```cpp
// _fetch_str — 通过页表将用户态字符串拷入内核缓冲区
int SyscallHandler::_fetch_str(uint64 addr, eastl::string &buf, uint64 max)
{
    proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
    mem::PageTable *pt = p->get_pagetable();
    int err = mem::k_vmm.copy_str_in(*pt, buf, addr, max);
    if (err < 0)
        return err;
    return buf.size();
}

// _arg_raw — 从陷阱帧寄存器中读取第 arg_n 个参数的原始值
uint64 SyscallHandler::_arg_raw(int arg_n)
{
    proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
    switch (arg_n)
    {
    case 0: return p->get_trapframe()->a0;
    case 1: return p->get_trapframe()->a1;
    case 2: return p->get_trapframe()->a2;
    case 3: return p->get_trapframe()->a3;
    case 4: return p->get_trapframe()->a4;
    case 5: return p->get_trapframe()->a5;
    }
    panic("arg_n is out of range");
    return -1;
}

// _arg_int / _arg_long — 在 _arg_raw 基础上做范围检查，窄化为目标类型
int SyscallHandler::_arg_int(int arg_n, int &out_int)
{
    int raw_val = _arg_raw(arg_n);
    if (raw_val < INT_MIN || raw_val > INT_MAX) // 范围校验
        return -1;
    out_int = (int)raw_val;
    return 0;
}
int SyscallHandler::_arg_long(int arg_n, long &out_int)
{
    // 结构同 _arg_int，校验上下界为 LONG_MIN / LONG_MAX
    // ...
}

// _arg_addr — 取出用户态地址，校验不超过最大虚拟地址
int SyscallHandler::_arg_addr(int arg_n, uint64 &out_addr)
{
    uint64 raw_val = _arg_raw(arg_n);
    if (raw_val >= MAXVA)
        return -EFAULT;
    out_addr = raw_val;
    return 0;
}

// _arg_str — 先取地址，再调用 _fetch_str 将用户态字符串拷贝到内核
int SyscallHandler::_arg_str(int arg_n, eastl::string &buf, int max)
{
    uint64 addr;
    if (_arg_addr(arg_n, addr) < 0 || addr == 0)
        return -EFAULT;
    return _fetch_str(addr, buf, max);
}

// _arg_fd — 取出 fd 编号，查进程 fd 表获取 file*，含悬空指针检测
int SyscallHandler::_arg_fd(int arg_n, int *out_fd, fs::file **out_f)
{
    int fd;
    fs::file *f;
    if (_arg_int(arg_n, fd) < 0)
        return -1;
    if (fd < 0 || (uint)fd >= proc::max_open_files)
        return SYS_EBADF;

    proc::Pcb *p = (proc::Pcb *)Cpu::get_cpu()->get_cur_proc();
    f = p->get_open_file(fd);
    if (f == nullptr)
        return SYS_EBADF;

    // 悬空指针检测：若文件对象已失效，自动清理 fd 槽位
    if (!is_probably_live_file_object(f))
    {
        // 将 fd 槽位置空并清除 close-on-exec 标志
        // ...
        return SYS_EBADF;
    }
    if (out_fd)
        *out_fd = fd;
    if (out_f)
        *out_f = f;
    return 0;
}
```
=== 系统调用表初始化

系统调用表是一个以调用号为索引的函数指针数组，容量为 2048 项，每个槽位指向一个系统调用处理函数。初始化时，所有槽位先填入默认处理函数（直接返回 `-ENOSYS`，表示未实现），随后通过 `BIND_SYSCALL` 宏将已实现的调用号逐一绑定到对应的处理函数上。绑定借助 C++ 的符号拼接机制，自动将系统调用号常量与同名处理函数关联，同时将调用名称存入并行数组供调试诊断使用。整个初始化过程在内核启动的 `main()` 函数中完成。

```cpp
void SyscallHandler::init()
{
    fs::init_bsd_flock_table();
    g_notify_registry_lock.init("notify registry");
    for (auto &func : _syscall_funcs)
    {
        // 默认实现
        func = &SyscallHandler::_default_syscall_impl;
    }
    // 初始化系统调用名称
    for (auto &name : _syscall_name)
    {
        name = nullptr;
    }
    BIND_SYSCALL(fork);
    BIND_SYSCALL(wait);
    BIND_SYSCALL(kill);
    BIND_SYSCALL(sleep);
    BIND_SYSCALL(uptime);
    //……
}
```

=== 系统调用分发器

系统调用分发器 `invoke_syscaller()` 是串联整个执行路径的中枢。它从当前进程的陷阱帧中读出调用号，以调用号为索引在 `_syscall_funcs[]` 中查找处理函数并调用，执行完毕后将返回值写入陷阱帧的 `a0` 位置。返回前还会校验进程页表基址的合法性，若发现页表损坏则立即触发内核 panic，防止内存错误扩散。

```cpp
void SyscallHandler::invoke_syscaller()
{
    proc::Pcb *p = (proc::Pcb *)proc::k_pm.get_cur_pcb();
    uint64 sys_num = p->get_trapframe()->a7;      // 从陷阱帧取调用号

    // 调用号越界或未绑定：打印错误并返回 -1
    if (sys_num >= max_syscall_funcs_num || sys_num < 0 ||
        _syscall_funcs[sys_num] == nullptr)
    {
        // ... 错误日志输出
        p->_trapframe->a0 = -1;
    }
    else
    {
        uint64 ret = (this->*_syscall_funcs[sys_num])();  // 通过函数指针调用
        // 校验页表基址，防止内存损坏扩散
        mem::PageTable *pt = p->get_pagetable();
        if (pt != nullptr && !is_sane_user_pagetable_base(pt->get_base()))
        {
            panic("pagetable base corrupted after syscall %s(%d), ...",
                  _syscall_name[sys_num], sys_num);
        }
        p->_trapframe->a0 = ret;                   // 返回值写入陷阱帧
    }
}
```

== 8.3　系统调用实现

F7LY 的系统调用实现覆盖了进程管理、内存管理、文件系统、信号、时间与定时器、Futex、事件通知、网络、权限、系统信息等十个功能类别。按照功能层次，实现代码分为接口数据定义、I/O 数据搬运、进程凭据管理三个部分，同时将设备控制与内核子系统逻辑独立出来，形成清晰的模块边界。

=== 接口数据定义

内核与用户态之间传递的结构体必须保持布局一致，否则 `copy_in`/`copy_out` 会导致数据错位。这一层集中定义了 Linux ABI 中用户态可见的数据结构，包括 `epoll_event`（16 字节，含 4 字节对齐空洞）、`termios`（36 字节，含控制字符数组）、`sigevent`（64 字节，含信号/线程通知联合体）、`timex`（208 字节，含时间校准参数）等，确保跨 RISC-V 和 LoongArch 架构的兼容性。

=== I/O 数据搬运

大量系统调用（如 `readv`、`writev`、`sendfile`、`splice`）涉及内核与用户态之间的数据往返。这一层提供了栈/堆双路径的临时缓冲区（小数据用栈上内联数组避免堆分配，大数据动态分配合并释放），vectored I/O 的散聚读写支持，以及优先尝试文件直拷快路径、失败时回退到内核中转的双策略。

=== 进程凭据管理

进程的 uid/gid 系列、supplementary groups、session/pgid、umask 和 personality 等属性修改都集中在这一层，包含完整的 POSIX 权限检查逻辑（特权进程可任意设置，非特权进程只能切换到 real/effective/saved 身份之一）和 capability 联动更新。

=== 设备控制逻辑

文件描述符的读写权限判定与 O_DIRECT 对齐校验、socket ioctl 的兼容接口模拟（如 SIOCGIFCONF 返回 loopback 接口视图）、块设备预读窗口大小的查询与设置，这些跨领域的设备控制逻辑被抽离为独立的管理类，不再嵌入系统调用处理函数中。

=== 内核子系统

三个功能完整的子系统——POSIX capability 管理（effective/permitted/inheritable 三集合的获取与设置）、内核时间校准（adjtimex/clock_adjtime 的状态机）、终端配置（termios 的获取/设置与线路规程同步）——各自维护独立的内部状态，系统调用处理函数仅作为入口将用户请求转发到子系统方法。

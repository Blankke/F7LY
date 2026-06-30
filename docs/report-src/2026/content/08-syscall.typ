= 系统调用

== 系统调用概述

系统调用是用户程序进入内核服务的统一入口。F7LY 实现了与 Linux raw syscall 约定兼容的接口，覆盖进程管理、内存管理、文件系统、信号、时间、futex、事件通知、网络、权限和系统信息等功能类别。完整实现列表放在附录，本章只说明系统调用层如何分发请求、搬运参数并连接内核子系统。

#figure(
  image("fig/系统调用.png", width: 70%),
  caption: [系统调用执行流程],
) <fig:syscall>

用户态程序把系统调用号和参数放入约定寄存器后执行陷入指令。架构 trap 入口保存现场并识别这是系统调用，再交给统一分发器处理。分发器根据调用号找到处理逻辑，执行完成后把返回值写回用户态返回寄存器。内核侧统一返回 Linux 风格错误码；表内未实现调用会落到默认处理并返回 `-ENOSYS`，调用号越界或表项异常时当前分发器兜底返回 `-1`。

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

== 执行流程

系统调用的一次执行可以分为四步：

1. *用户态陷入*：用户程序按照 ABI 放置调用号和最多六个参数，执行陷入指令进入内核。
2. *trap 分发*：架构入口判断异常类型，推进返回地址，打开允许的中断窗口，并转入系统调用分发器。
3. *查表执行*：分发器检查调用号范围和绑定状态，调用对应处理逻辑；处理逻辑只通过安全参数接口访问用户态地址和 fd。
4. *返回用户态*：返回值写回陷阱帧，随后内核处理待投递信号、调度抢占和用户态现场恢复。

这个流程把架构差异限制在 trap 入口和寄存器约定中，系统调用实现本身尽量复用同一套 C++ 逻辑。

== 分发表设计

F7LY 使用固定容量的系统调用表作为分发核心。初始化时，表中所有槽位先指向默认未实现处理；随后按系统调用号绑定已实现入口。调用名称也随绑定记录下来，用于诊断输出和日志定位。

这种设计有三个好处。第一，表内未实现调用有统一兜底，不会落入空指针。第二，新增系统调用只需要同步编号、声明、实现和绑定位置，入口路径保持一致。第三，RISC-V 与 LoongArch 共用同一张逻辑表，保证两架构看到的 Linux ABI 编号一致。

分发器本身只做边界检查、入口调用、返回值写回和少量一致性检查。具体语义放在各系统调用处理逻辑或内核子系统中，避免把 VFS、进程、内存、网络等细节堆在分发层。

== 参数与用户内存

系统调用参数先按原始寄存器值读取，再根据目标类型进行解释。整数参数按目标类型提取，地址参数只作为用户虚拟地址保存，字符串和结构体必须通过页表感知的拷贝函数搬入内核，输出数据则通过对称的拷出函数写回用户态。内核不会直接解引用用户指针。

文件描述符参数需要额外经过 fd 表校验。内核先检查 fd 编号范围，再取得当前进程的文件对象，并确认对象仍然有效。这样系统调用处理逻辑拿到的是已经验证过的 `file` 抽象，可以直接转交 VFS、socket、pipe、eventfd 或设备后端。

这一层的核心原则是“先校验边界，再进入子系统”。无效地址返回 `-EFAULT`，非法 fd 返回 `-EBADF`，参数不符合语义返回对应 Linux errno。错误码在系统调用层保持负值，用户态 C 库再按约定转换成 `errno`。

== 实现分层

系统调用实现不是按文件简单堆叠，而是围绕几类职责拆分：

- *ABI 数据结构*：定义用户态可见结构体的布局、对齐和大小，保证 RISC-V 与 LoongArch 下 `copy_in` / `copy_out` 解释一致。
- *进程与凭据*：处理 pid/tid、会话、进程组、uid/gid、capability、资源限制和 personality 等进程属性。
- *内存接口*：把 `brk`、`mmap`、`munmap`、`mprotect`、共享内存和 memfd 请求转交地址空间与页后端对象。
- *文件与 I/O*：把 open/read/write/stat/mount/ioctl/fcntl/xattr 等请求转交 VFS、文件对象和设备控制层。
- *信号、时间与同步*：连接信号处理表、POSIX timer、futex、epoll、eventfd 和等待唤醒机制。
- *网络与设备控制*：socket 系列系统调用进入网络子系统，块设备、终端和 socket 的 ioctl 按设备类型分发。

系统调用处理函数在这一分层中扮演“ABI 边界适配器”的角色：它负责把用户态参数转换成内核对象和安全缓冲区，再调用真正持有状态的子系统。这样修改某个子系统的内部实现时，不需要改变所有系统调用入口。

== 新增系统调用的约束

新增系统调用时需要同步四类信息：Linux ABI 编号、内核侧声明、处理逻辑和分发表绑定。若用户态封装或自动入口需要调用它，还要同步用户态 wrapper。实现时应优先复用已有的参数获取、fd 校验、用户拷贝和 errno 风格，避免每个系统调用各自实现一套边界检查。

对于已经有子系统承载的能力，系统调用层只做入口适配，不在分发文件里重写业务逻辑。例如文件路径解析交给 VFS，页映射交给地址空间管理，事件等待交给 epoll/file 就绪判断，设备命令交给对应 ioctl 模块。这是保持系统调用层可维护的主要原则。

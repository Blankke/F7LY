= 系统调用

== 调用入口与 ABI

=== 架构 trap 到公共入口

RISC-V 在 `a7` 读取 syscall number、在 `a0..a5` 读取参数，系统调用异常返回前将 `sepc` 加 4；LoongArch 在 `r4`/约定寄存器读取参数，使用 `era += 4` 返回。两种架构的 trap 代码只负责保存现场和进入 `SyscallHandler::invoke_syscaller()`，编号、参数和返回值的公共处理集中在 syscall 层。

```cpp
void SyscallHandler::invoke_syscaller() {
  auto *tf = current_trapframe();
  uint64 nr = syscall_number(tf);
  uint64 args[6] = {arg0(tf), arg1(tf), arg2(tf),
                    arg3(tf), arg4(tf), arg5(tf)};
  tf->a0 = dispatch(nr, args);
}
```

用户态 wrapper 只依赖架构规定的寄存器和负 errno 约定，内核不把某个 libc 的私有 wrapper 当作 syscall 语义来源。

=== 编号表与默认行为

`SyscallHandler::init()` 先把全部槽位填为默认实现，再使用 `BIND_SYSCALL(name)` 显式绑定已实现接口和名称。未知编号统一返回 `-ENOSYS`，避免未初始化函数指针跳转；新增接口必须同步 `syscall_defs.hh`、handler 声明、实现和绑定表，涉及用户封装时再同步对应 wrapper。

== 参数、用户内存与错误码

=== 参数获取与对象校验

内核先检查整数参数的范围、flags 组合和长度溢出，再解析 fd、pid、timerid 或 socket 对象。fd 查找返回带引用的 file 对象，调用结束后释放临时引用；路径参数和用户结构体不能直接解引用，必须通过 VMM 的 copyin/copyout 访问。

```cpp
int copy_stat_to_user(uint64 user_addr, const Kstat &kst) {
  if (!is_user_writable(user_addr, sizeof(kst)))
    return -EFAULT;
  return mem::k_vmm.copy_out(current_pagetable(),
                             user_addr, &kst, sizeof(kst));
}
```


=== Linux errno 与结构体布局

errno 决定失败时用户态收到的错误类型，结构体布局决定成功时用户态如何解析内核写回的数据。它是 syscall 入口与用户程序之间的结果接口，直接影响 glibc 对调用结果的判断。

内核内部错误统一转换为负的 Linux errno，例如 `-EINVAL`、`-EFAULT`、`-EBADF`、`-ENOENT` 和 `-ENOSYS`。向用户写回的结构体使用 Linux ABI 的字段宽度、对齐和时间单位，不能因为内核内部使用不同类型就直接 memcpy 未转换的数据。

== 系统调用分发与内核模块调用

=== 统一分发表

当前 syscall 表由一个 handler 统一维护，先根据 syscall 编号找到处理函数，再由处理函数调用对应的内核对象。这样可以保持统一 ABI，同时避免每个 syscall 重新实现资源生命周期。

=== 系统调用与内核模块的对应关系

进程和线程调用 `ProcessManager`，内存调用 `ProcessMemoryManager`/VMM，文件调用 VFS/file，IPC 调用 futex、pipe、shm 和 epoll，网络调用 socket_file/ONPS，时间调用 timer/clock backend。系统调用层负责参数和返回值，具体模块负责状态变化、锁和资源回收。

== 新增系统调用的约束

新增 syscall 需要同时完成以下工作：

- 在 syscall 定义中固定 Linux 编号和 ABI 结构体；
- 在 handler 中显式绑定，未实现路径保持 `-ENOSYS`；
- 校验所有用户指针、长度、flags、fd 和权限；
- 更新相关内核对象的引用、锁和失败回滚；
- 为 RISC-V/LoongArch glibc wrapper 和最小回归场景提供验证。


== 验证结果

- RV/LA 四种画像构建和 glibc CAgent 链路可使用统一 syscall 表。
- 双架构 78 项定向 glibc 回归覆盖 clone、futex、mmap、shm、时间、文件和 epoll 组合，未出现 syscall panic 或异常返回路径。
- syscall 参数校验、用户内存 copy、负 errno 和 fd 生命周期与 VFS、内存、IPC 章节的对象模型保持一致。

== 本章小结

本阶段的 syscall 层从“集中函数表”推进为“统一 ABI 入口加内核模块调用”：架构 trap 提供寄存器调用约定，handler 负责分发和参数边界，各内核子系统负责状态与资源生命周期，最后以 Linux errno 和 ABI 结构体向用户返回结果。

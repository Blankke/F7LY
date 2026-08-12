= 进程间通信

决赛阶段的 IPC 重点，是让线程和进程在多核并发下能够可靠地等待、唤醒、传递数据并观察事件。BuildStorm 中的 cargo、rustc 和 build script 同时使用 futex、管道、信号、共享映射和 epoll；因此本章按“同步—数据—共享内存—事件通知”的关系组织，而不是简单罗列系统调用。

== 同步等待与唤醒

=== futex key 与等待队列

futex 不直接以用户虚拟地址作为全局键。内核根据私有/共享属性把地址转换为进程私有键或物理页键，再在固定 bucket 中登记等待者。等待者保存 key、期望值、超时结果和所属 PCB，wakeup 只扫描匹配 key 的候选任务。

WAIT 路径必须在发布等待状态前重新读取用户值：

```cpp
lock(futex_bucket);
lock(current_pcb);
if (load_user_word(uaddr) != expected) {
  unlock(current_pcb);
  unlock(futex_bucket);
  return -EAGAIN;
}
publish_futex_key(current_pcb, key);
sleep_on_key(current_pcb, key);
unlock(current_pcb);
unlock(futex_bucket);
```

WAKE 使用相同的锁顺序筛选并唤醒匹配任务。这样既避免一个 CPU 持 futex 锁等待 PCB 锁、另一个 CPU 反向持锁造成的 ABBA，也避免在“检查用户值”和“登记等待者”之间丢失唤醒。

=== 超时、信号与 robust list

futex 超时由定时器提供唤醒机会，线程被唤醒后仍重新检查 futex 状态，区分正常唤醒、超时和信号中断。线程退出时遍历用户态 robust list，对仍由该线程持有的 robust futex 设置 owner-died 状态并唤醒等待者，保证 pthread 锁不会永久占用。

=== requeue 与锁语义

条件变量常通过 `FUTEX_REQUEUE` 将等待者从一个 bucket 转移到 mutex bucket。转移过程保持 key、等待结果和锁顺序一致，避免先唤醒后重排导致的重复唤醒。`CLONE_CHILD_CLEARTID` 退出清零也复用 futex wake，使 pthread join 能够观察线程退出。

== 管道与 FIFO

=== 管道数据通道

管道以环形缓冲保存字节流，读端和写端分别维护引用。空管道上的 read、满管道上的 write 根据阻塞标志睡眠；写入数据后唤醒读者，读出空间后唤醒写者。关闭最后一个读端或写端时返回 Linux 约定的 EOF/EPIPE，并检查信号以便阻塞操作能够退出。

=== FIFO 与路径对象

FIFO 在 VFS 中由 FIFO manager 维护同一路径对应的读写端配对状态，打开操作可以等待另一端出现。底层数据传输仍复用 pipe file 的读写和 sleep/wakeup 逻辑，避免路径对象和匿名管道出现两套阻塞语义。

== 共享内存与 memfd

=== SysV 共享内存

ShmManager 保留 key、权限、attach/detach 和删除标记等 IPC 元数据，实际页后端交给 `VmObject`。多个进程 attach 同一段共享内存时，VMA 描述映射区间，VmObject 维护页面和引用计数，页表层统一安装 PTE。这样共享内存与普通匿名映射、文件映射使用一致的缺页和回收路径。

=== memfd 与 sealing

memfd 创建匿名文件对象并通过 `/proc/self/fd` 暴露 fd 视图。其大小、共享映射和 seal 状态由同一 memfd state 维护；`F_SEAL_WRITE`、`F_SEAL_GROW` 和 `F_SEAL_SHRINK` 分别约束写入、扩展和收缩，mmap、truncate、fallocate 在执行前检查 seal，避免只在单个 syscall 中做局部判断。

== 事件通知

=== epoll 就绪模型

epoll file 保存被监听 fd 与事件掩码，普通 file、pipe、socket、virtual file 和 eventfd 通过 `read_ready()`/`write_ready()` 参与统一就绪检查。wait 路径先扫描 ready 状态，再结合 signal mask 和 timeout 决定返回或睡眠。

零超时路径不分配用户页，也不进入可睡眠路径；使用固定栈缓冲和轮转游标收集事件：

```cpp
int wait_ready(EpollFile &ep, Event *out, int maxevents) {
  int count = 0;
  for (FdEntry &entry : ep.round_robin_entries()) {
    if (entry.file->ready(entry.events))
      out[count++] = entry.event;
    if (count == maxevents)
      break;
  }
  return count;
}
```

轮转游标避免低编号 fd 长期占满返回数组；LT 事件保持可重复观察，ET 事件只在新状态或尚未交付时报告，从而兼顾公平性和 Linux 事件语义。

=== eventfd 与其他就绪对象

eventfd 用 64 位计数器实现线程间通知：写入增加计数，读取按 semaphore 或计数器模式减少计数，计数为零时不可读，接近上限时不可写。它与 pipe、socket、timerfd 一样通过 file 对象的 ready 接口接入 epoll，不在 epoll 中维护重复的设备状态。

=== POSIX timer 的事件交付

POSIX timer 的创建、设置、查询、删除、到期和进程退出清理由同一 timer lock 保护。周期定时器根据当前时间一次性计算 overrun，信号投递前发布 overrun 值，`timer_getoverrun()` 可以读取同一状态；无已武装定时器时，timer tick 不扫描全部 PCB。

== 验证结果

- RV/LA 的 futex wait/wake、bitset、requeue、robust list 和 clone301/302 定向场景保持正常。
- `epoll_wait04` 连续 10 次、POSIX timer 的 delete/gettime/settime、clock_settime、getitimer 等组合回归返回 0，零超时路径没有页分配或异常睡眠。
- pipe/FIFO、SysV SHM、memfd sealing、eventfd 与 mmap 共享路径使用统一 file/VmObject 生命周期，未出现重复释放或用户态可见状态不一致。

== 本章小结

本阶段将 IPC 组织为“futex/睡眠队列负责同步，pipe/FIFO 负责字节流，VmObject/memfd 负责共享页面，epoll/eventfd/timer 负责事件观察”的分层模型。所有对象都通过明确的引用、锁序和 ready 接口连接到进程、VFS 和内存管理，能够支撑多线程编译和 Linux ABI 回归。

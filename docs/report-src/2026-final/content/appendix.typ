= 附录

本附录提供正文中使用的复现入口、平台组合和关键 syscall 语义索引。它不重新列出所有系统调用，而是帮助读者确认某项结论对应的构建方式、测试范围和语义变化。

== 附录 A　阶段改动索引 <sec:appendix-change-index>

#table(
  columns: (1.5fr, 3fr, 1fr),
  table.header([*主题*], [*主要模块*], [*正文位置*]),
  [机器启动与平台分层], [`kernel/boot`、`kernel/platform`、`mk/platform`、`BootInfo`], [第二章],
  [中断与跨核通知], [`kernel/trap`、IRQ registry、IPI/timer backend], [第三章],
  [物理与虚拟内存], [PMM、Buddy、`VMASpace/VMObject`、ASID、COW、TLB], [第四章],
  [进程与线程], [PCB、scheduler、clone/clone3、退出回收], [第五章],
  [文件系统], [VFS、ext4、bcache、block backend], [第六章],
  [进程间通信], [futex、pipe、SHM、epoll、eventfd、POSIX timer], [第七章],
  [系统调用与 ABI], [syscall handler、copyin/copyout、errno、ABI 结构体], [第八章],
  [网络系统], [socket_file、ONPS、net backend、`/proc/net/tcp{,6}`], [第九章],
)


== 附录 B　关键系统调用与行为变化 <sec:appendix-syscall-semantics>

下表列出 CAgent 和 BuildStorm 两条决赛工作链路反复依赖、且本阶段行为得到调整的系统调用。它不是完整系统调用清单。

#table(
  columns: (1.5fr, 2fr, 4fr),
  table.header([*依赖场景*], [*关键接口*], [*本阶段行为变化*]),
  [动态程序与并发进程], [`clone`、`clone3`、`execve`、`wait4`], [明确线程组、共享 `mm/files/sighand`、`clear_tid` 和 exec 失败回滚；创建失败不遗留 PCB 锁或引用。],
  [Rust 多线程编译], [`futex`、`sched_yield`、`tgkill`], [统一 WAIT/WAKE 锁序，按真实等待候选唤醒，避免 ABBA 和丢唤醒；线程退出后正确回收共享地址空间。],
  [大量地址空间操作], [`mmap`、`munmap`、`mprotect`、`mremap`、`madvise`、`brk`], [统一进入 `VMASpace/VMObject`，支持懒缺页、COW、共享映射、批量 teardown，并同步活跃 mm CPU 的 TLB。],
  [编译器文件访问], [`openat`、`read`、`write`、`pread`、`pwrite`、`getdents64`], [路径解析、目录 cookie、文件偏移和用户缓冲区保持 Linux 语义；普通文件最后关闭时提交合并写。],
  [ext4 元数据与缓存], [`fsync`、`fdatasync`、`fallocate`、`renameat2`、`linkat`、`unlinkat`、`statfs`], [写回、目录变更和挂载统计反映真实 ext4 状态，减少并发 close、bcache 和 mount lock 造成的不一致。],
  [事件与定时器], [`epoll_wait`、`eventfd`、`timerfd`、`timer_create`、`timer_settime`], [零超时路径不分配页，LT/ET 使用轮转游标保持公平；定时器归属、删除、到期和 overrun 由统一锁保护。],
  [系统环境查询], [`sysinfo`、`uname`、`getcpu`、`sched_getaffinity`], [返回 online CPU、真实 uptime、动态进程数、负载和架构名称，不用固定值替代内核状态。],
  [网络与状态观测], [`socket`、`bind`、`listen`、`connect`、`sendmmsg`、`recvmmsg`、`poll`], [TCP/UDP loopback 具备真实 payload 和阻塞语义；socket 生命周期登记生成 `/proc/net/tcp{,6}` 的 LISTEN/ESTABLISHED 等状态。],
)

这些变化分别对应第 4 至第 9 章的实现，证据来自双架构定向回归、CAgent 连续运行和 BuildStorm 阶段日志；环境型 `TCONF` 不作为内核语义通过计入。

== 附录 C　平台与构建矩阵 <sec:appendix-platform-matrix>

#table(
  columns: (3.6cm, 2.2cm, 1.8cm, 4.2cm, 3.4cm),
  table.header([*Profile*], [*架构*], [*运行方式*], [*主要设备组合*], [*典型用途*]),
  [#text(size: 8.5pt)[`riscv-qemu`]], [RISC-V], [QEMU], [PLIC、VirtIO MMIO], [evaluation、CAgent、BuildStorm],
  [#text(size: 8.5pt)[`loongarch-qemu`]], [LoongArch], [QEMU], [PCH/ExtIOI、VirtIO PCI], [evaluation、CAgent、BuildStorm],
  [#text(size: 8.5pt)[`riscv-visionfive2`]], [RISC-V], [实机], [PLIC、DWMMC/SD、GMAC1], [平台构建与板级适配],
  [#text(size: 8.5pt)[`loongarch-2k1000`]], [LoongArch], [实机], [LIOINTC、AHCI、GMAC、RTC], [平台构建与板级适配],
)

常用构建命令如下：

```bash
make build PROFILE=riscv-qemu
make build PROFILE=loongarch-qemu
make build PROFILE=riscv-visionfive2 MODE=shell
make build PROFILE=loongarch-2k1000 MODE=shell
```

QEMU 压力运行使用决赛磁盘和 8G/8 核参数：

```bash
make run PROFILE=riscv-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8
make run PROFILE=loongarch-qemu QEMU_DISK=final QEMU_MEM=8G QEMU_SMP=8
```

`PROFILE` 同时决定架构、平台、驱动集合和链接脚本；`MODE=evaluation` 用于回归入口，`MODE=shell` 用于交互式用户态。QEMU 长输出应保存到 `logs/run/output_*.txt`，并在日志头部记录命令、架构、commit 和退出码。

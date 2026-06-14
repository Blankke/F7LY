= 2026 年改动、删除与改进

== 从分散 VMA 修补到统一地址空间所有权

=== 改动前的问题

=== ProcessMemoryManager 的权威职责

=== Mmap、缺页、Fork、Clone、Exec 与退出回收

=== 改动效果与验证

== DTB 驱动的内存与启动布局

=== 固定 128 MiB 适配的局限

=== DTB、Initrd 与动态物理内存区间

=== PMM、Heap 与共享内存区域划分

=== 双架构启动验证

== LoongArch Trap、TLB 与 LL/SC 稳定化

=== ECODE 与中断 Pending 判定

=== Timer 中断与锁粒度

=== Usertrapret、Trapframe 与寄存器恢复

=== 多线程 LL/SC 与 TLB 约束

=== Pthread、Libcbench 与 Lmbench 结果

== 进程、线程、信号与 Futex 语义

=== PID、TID、TGID 与 Clone Flags

=== Mm、Files 与 Sighand 共享

=== Robust Futex 与 Clear Child TID

=== SIGCHLD、Wait Status 与进程组

=== 进程优先级与 I/O 优先级联动

== VFS、Ext4 与挂载语义

=== 根文件系统与多文件系统策略

=== File 对象体系

=== Mount、Umount 与 Bind Mount

=== 虚拟 Proc、Etc 与 Dev 文件

=== Fcntl、Ioctl、Splice、Xattr、Fanotify 与 Memfd

=== Ext4 Cache 与文件 I/O 优化

== 系统调用组织与 Linux ABI 语义

=== 系统调用表、参数与负 Errno

=== Syscall ABI、Sysio 与 Sysproc

=== 按领域拆分的管理对象

=== 新增与改动接口分类

== 性能与长回归优化

=== 块队列与 Descriptor 回收

=== Ext4、Buffer Cache 与批量 I/O

=== Exec 与测例启动加速

=== 信号和进程清理去重

=== Iozone、Libcbench 与回归耗时

== 构建、运行与仓库结构改进

=== 双架构构建与 QEMU 参数

=== Evaluation 与 Shell 模式

=== Debug、Scripts、Tools 与 Logs 目录

== 删除与替换

=== 删除旧 RISC-V VirtIO Block 接口

=== 删除 MClock 调度器

=== 删除递归符号链接解析

=== 迁移根目录旧工具入口

=== 删除误提交日志与重复测例

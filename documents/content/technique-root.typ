#import "../components/figure.typ": algorithm-figure, code-figure

= 技术路线
== Git
本节详述F7LY操作系统对现场赛核心应用Git的技术支持路径。Git作为分布式版本控制系统，其运行机制对操作系统的文件系统、内存管理和系统调用接口提出了严格要求。我们从程序行为分析入手，构建针对性的内核支持方案。
=== 内核支持

==== 目录扫描与文件状态检查
Git的核心操作如`git add`和`git status`需要递归扫描工作目录，识别文件变更状态。这一过程的实现依赖于高效的目录遍历机制：

- *`getdents64`系统调用*：Git使用该系统调用批量获取目录项信息，避免逐一调用`readdir`带来的性能损耗。F7LY内核实现了完整的`getdents64`支持，确保返回值格式严格符合POSIX标准，包括正确的`d_reclen`字段计算和`d_type`文件类型标识。

```cpp
struct linux_dirent64 *d;
struct linux_dirent64 *entry = (struct linux_dirent64 *);
```

- *文件状态查询优化*：Git频繁使用`faccessat`、`fstatat`等系统调用检查文件访问权限和元数据。F7LY针对这些调用优化了VFS层的属性缓存机制，减少底层文件系统的重复访问。

==== 文件操作与I/O管理
Git的文件操作模式对系统调用接口的正确性和性能提出了挑战：

- *原子文件操作*：`openat`系统调用是Git文件操作的基础，支持相对路径操作和目录文件描述符传递。F7LY实现了完整的`openat`族系统调用，确保路径解析的原子性和安全性。


==== 内存映射与懒分配机制
Git大量使用内存映射技术优化大文件处理和动态链接库加载：

- *文件映射支持*：Git通过`mmap`将索引文件、包文件等映射到虚拟地址空间，实现零拷贝访问。F7LY实现了完整的文件映射功能，支持`MAP_PRIVATE`、`MAP_SHARED`等映射模式，并通过页表机制实现写时拷贝（COW）。

```c
// Git中典型的文件映射模式
fs::file *f = nullptr;
proc::Pcb *p = proc::k_pm.get_cur_pcb();
// 直接通过指针访问文件内容
```

- *懒分配策略*：针对Git的大文件映射需求，F7LY实现了懒分配（lazy allocation）机制。仅在实际访问时分配物理页面，显著降低内存占用和启动时间。

- *动态链接库映射*：Git依赖多个共享库，F7LY在`execve`实现中正确解析ELF文件的动态段，建立准确的库文件映射，确保符号解析和重定位的正确性。

==== 原子性操作保障
Git使用锁文件机制保证并发安全，这对内核的原子操作支持提出了严格要求：

Git通过创建`.lock`临时文件，写入完成后使用`renameat2`原子性重命名来更新`index`、`config`等关键文件。F7LY实现了完整的`renameat2`系统调用，支持`RENAME_NOREPLACE`等标志位，确保操作的原子性。

```c
// Git中的原子更新模式
int lock_fd = openat(dir_fd, "index.lock", O_CREAT | O_EXCL | O_WRONLY, 0644);
// 写入新内容到lock文件
write(lock_fd, new_data, data_size);
fsync(lock_fd);
close(lock_fd);
// 原子性重命名替换原文件
renameat2(dir_fd, "index.lock", dir_fd, "index", RENAME_NOREPLACE);
```

=== 针对性优化

=== 系统调用流程优化
F7LY针对Git的使用模式进行了深度优化：

- *`getdents64`流程完善*：通过分析Git的目录扫描模式，F7LY优化了目录项缓存策略，减少磁盘I/O次数。同时规范化了返回值处理，确保与glibc的`readdir`实现完全兼容。

- *批量状态查询*：针对Git频繁的文件状态检查，F7LY实现了批量`stat`操作优化，通过单次系统调用获取多个文件的元数据，减少用户态与内核态切换开销。

=== ELF文件与动态链接支持
Git作为复杂的用户态程序，其正确运行依赖于完善的动态链接支持：

- *ELF解析规范化*：F7LY在`execve`系统调用中实现了严格的ELF文件解析流程，正确处理程序头表、节头表和动态段信息。确保各类段（`.text`、`.data`、`.bss`等）的内存布局与权限设置符合ABI规范。

- *动态链接库装载*：通过解析ELF文件的`PT_INTERP`段获取动态链接器路径，正确建立链接器与目标程序的内存映射关系。同时处理`PT_DYNAMIC`段中的依赖库信息，确保符号解析的正确性。

- *内存布局一致性*：为保证QEMU环境与物理硬件的一致性，F7LY统一了虚拟地址空间布局，确保动态链接库的加载地址和权限设置在不同平台上保持一致。

#text()[#h(2em)]这些技术措施的综合实施，为Git在F7LY系统上的稳定运行提供了坚实的内核基础，同时为其他复杂用户态程序的支持奠定了技术框架。

= 文件系统

== 块设备与文件系统边界

=== 公共块设备层

VFS 不直接访问 VirtIO、AHCI 或 SD 控制器，而是经过公共 block backend。RISC-V QEMU 使用 VirtIO MMIO，LoongArch QEMU 使用 VirtIO PCI，2K1000 使用 AHCI，VisionFive2 使用 JH7110 DWMMC；平台 backend 只提供扇区读写和设备容量，公共块层负责识别裸 ext4、MBR 和 GPT。

VirtIO block 驱动改为读取设备真实 capacity，不再假设固定 4 GiB。分区偏移也只由公共块层维护，驱动不会把某个镜像的分区布局复制到 VFS 或文件系统对象中。

=== ext4 挂载路径

挂载阶段先建立 block device，再创建 ext4 mount、buffer cache 和 VFS 文件系统对象。lwext4 的非线程安全 bcache/间接块访问由 mount guard 串行化，直接数据块 I/O 则在不持有全局排他锁的条件下进行，避免读路径和大文件写回互相阻塞。

== ext4 并发与缓存

=== 可重入 mount lock

ext4 的低层操作可能递归获取 inode、目录和 block cache 状态。本阶段使用记录持有者 PCB 和递归深度的 FIFO 睡眠锁，并在进入直接 `ext4_fs_get_inode_ref()` 的 VFS 路径时安装 `Ext4MountGuard`。同线程嵌套调用只增加深度，其他线程按 FIFO 睡眠，释放时精确唤醒下一个等待者。

=== bcache 的 dirty/LRU 管理

块缓存同时维护 LRU 链、按 LBA 的查找树和 dirty 链。dirty 链改为双向 TAILQ，插入和删除为 O(1)；缓存描述符从动态池取得，避免高并发下频繁分配内核 heap。引用计数、重复释放和链表指针损坏改为现场断言，使缓存一致性错误尽早暴露。

```cpp
void insert_dirty(ext4_buf *buf) {
  ext4_assert(buf->refctr > 0);
  ext4_assert(is_dirty(buf) && is_uptodate(buf));
  if (!buf->in_dirty_list) {
    TAILQ_INSERT_TAIL(&bc->dirty_list, buf, dirty_node);
    buf->in_dirty_list = true;
  }
}
```

写回路径在 buffer 仍被引用时不回收它；最后一个引用释放后，根据 write-back 状态决定保留、刷盘或从 LRU 淘汰，避免已关闭文件重新看到旧 inode EOF。

=== 写回、fsync 与最后关闭

完整块写和小块写都先进入 bcache 并标记 dirty，再由 write-back 或显式 fsync/fdatasync 提交到底层设备。普通文件最后一次 close 会刷新合并写和 inode 可见性，确保后续打开、stat、目录遍历和其他进程读取到相同的文件大小与内容。

== 路径解析与目录操作

=== 组件缓存

目录项增删路径维护正向和负向组件缓存。命中正缓存时直接得到 inode，命中负缓存时可快速返回 ENOENT；create、rename、link、unlink 和目录 rename 会从权威目录项操作位置使相关区间失效，避免缓存长期保留旧类型。

根目录打开文件时合并每级前缀的 resolve 与 stat，symlink 解析使用单遍扫描，不再为每个组件创建临时 vector。这样可以减少 Rust 编译期间反复访问 `/work`、`/tmp` 和工具链目录的路径开销。

=== getdents64 与目录位置

目录文件的 `_file_ptr` 使用文件系统返回的目录 cookie 更新，而不是简单累加用户缓冲区长度。`getdents64` 因此能够在短缓冲、多次读取和并发目录搜索中保持稳定的遍历位置，目录项类型也由 ext4 真实 inode 类型填充。

=== 特殊文件语义

`O_TMPFILE` 创建的隐藏目录项在最后关闭时删除；`/proc`、`/dev`、`/etc` 等虚拟节点由 VirtualFileSystem 提供 backing 优先的视图。普通文件、目录、设备文件、pipe、socket、virtual file 和 epoll file 通过统一 file 对象接口参与 fd 查找和引用计数。

== 文件操作与 Linux 语义

=== 读写与偏移

normal file 的 read/write 与 pread/pwrite 共用文件对象和页缓存，但分别维护共享文件偏移与显式偏移。ftruncate、fallocate、rename、link、unlink 和 getdents64 更新 inode、目录项和缓存状态时遵循同一 mount guard，避免目录内容和文件大小短暂不一致。

=== statfs/fstatfs

`statfs` 根据路径找到实际挂载，`fstatfs` 根据 fd 找到所属文件系统，再从 ext4 superblock 读取 blocks、free blocks、available blocks、inode 数量、fsid、文件名长度和挂载标志；路径不存在、fd 类型错误和用户地址无效分别返回对应 Linux errno，不使用固定伪造统计值。

=== relatime 与元数据写放大

普通读路径采用 relatime 条件更新 atime：只有超过时间间隔或早于 mtime/ctime 时才写回 inode。数据读取完成后再进行短暂的排他元数据更新，减少共享读路径中的 atime 写放大，同时保留用户可观察的时间语义。

== 验证结果

- RISC-V/LoongArch 的 VirtIO、AHCI、DWMMC block backend 均可进入公共 VFS；VirtIO capacity、分区识别和 ext4 挂载路径不依赖固定镜像大小。
- 双架构 CAgent 文件创建、读写、目录遍历和搜索均可完成；`statfs02/fstatfs02`、pread/pwrite、ftruncate、fallocate、fsync、rename、link、unlink 和 getdents64 定向回归保持正常。
- ext4 并发短测覆盖 write/fsync/rename/read/unlink，临时镜像只读 e2fsck 未发现结构损坏；路径缓存、relatime 和 close 写回优化通过对应窄测。

== 本章小结

本阶段将文件系统路径收敛为“平台 block backend—公共块层—ext4/bcache—VFS file 对象”的结构，并用可重入挂载锁、O(1) dirty 管理、组件缓存和最后关闭写回解决决赛并发访问中的一致性与性能问题。上层进程、内存和网络章节可以直接复用这些统一的文件对象和块设备语义。

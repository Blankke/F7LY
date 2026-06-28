= 第六章　文件系统

== 设备抽象层

=== 虚拟设备基类的设计理念

虚拟设备基类（`VirtualDevice`）是设备抽象层的根基，采用面向对象设计中的抽象基类模式，为所有具体设备类型提供统一的接口规范。该基类定义了四类纯虚接口，涵盖设备类型识别、中断处理、读写就绪状态检查等核心能力：

```cpp
namespace dev
{
    enum DeviceType { dev_unknown, dev_block, dev_char, dev_other };

    class VirtualDevice
    {
    public:
        virtual DeviceType type() { return dev_unknown; }
        virtual int handle_intr() = 0;     // 中断处理入口
        virtual bool read_ready() = 0;     // 读就绪查询
        virtual bool write_ready() = 0;    // 写就绪查询
    };
}
```

四类接口各司其职：`type()` 返回设备类型枚举，供设备管理器在运行时区分钟别；`handle_intr()` 是中断处理的统一入口，由 trap 分发路径调用；`read_ready()` 和 `write_ready()` 则提供就绪状态查询，为上层 VFS 和 epoll 的事件通知体系提供统一的判断依据——epoll 最终调用 `file::read_ready()`，而 `device_file` 将其直接转发至 `VirtualDevice::read_ready()`，整条就绪通知链路由此贯通。

`VirtualDevice` 按 RISC-V 和 LoongArch 两个架构分别定义，但接口声明完全一致。`BlockDevice` 等派生类通过条件编译选取对应架构的头文件，对外不感知差异。

从 `VirtualDevice` 出发，设备继承体系分为两支：字符设备支线 `CharDevice : public VirtualDevice`，其下派生出 `StreamDevice`（流设备适配层）、`UartManager`（串口驱动）和 `LoopControlDevice`（loop 控制面）；块设备支线 `BlockDevice : public VirtualDevice`，其下注册了 `RamDisk`（内存盘）和 `DiskPartitionDevice`（MBR 分区代理），而 virtio-blk 由于已统一收口到 §6.2 的独立框架，不再作为 `BlockDevice` 的子类出现。

=== 字符设备的特性与应用

字符设备类（`CharDevice`）在基类接口之上扩展了面向字符的读写操作。其设计源于 Unix 经典的字符设备模型，适用于控制台输入输出、串口通信等逐字符交互的场景。

```cpp
class CharDevice : public VirtualDevice
{
public:
    virtual DeviceType type() override { return dev_char; }
    virtual bool support_stream() = 0;       // 是否支持流式操作
    virtual int get_char_sync(u8 *c) = 0;    // 同步读一个字符
    virtual int get_char(u8 *c) = 0;         // 异步读一个字符
    virtual int put_char_sync(u8 c) = 0;     // 同步写一个字符
    virtual int put_char(u8 c) = 0;          // 异步写一个字符
    virtual int handle_intr() = 0;

    // 缓冲区管理 —— 提供默认实现，子类按需覆盖
    virtual int get_input_buffer_size() { return 0; }
    virtual int get_output_buffer_size() { return 0; }
    virtual int flush_buffer(int queue) { return 0; }
    virtual int get_line_status() { return 0x01; }
};
```

=== 块设备的架构与优化

块设备代表了 F7LY 中专门用于大容量数据存储和批量数据传输的设备类别。与字符设备的流式处理方式不同，块设备以固定大小的数据块为基本操作单位。

```cpp
class BlockDevice : public VirtualDevice
{
public:
    virtual DeviceType type() override { return dev_block; }
    virtual long get_block_size() = 0;
    virtual int read_blocks(long start_block, long block_count,
                            BufferDescriptor *buf_list, int buf_count) = 0;
    virtual int write_blocks(long start_block, long block_count,
                             BufferDescriptor *buf_list, int buf_count) = 0;
    // ……
};
```

`BlockDevice` 在基类接口之上提供了同步和异步两种块操作模式——文件系统在中断上下文中可使用异步版本，在同步写路径则使用阻塞版本。此外，`BufferDescriptor` 机制允许单次 I/O 请求携带多个缓冲描述符，数据可分散读入多个不连续内存区域或从多处集中写出（scatter-gather），有效减少了内核在 I/O 路径上的额外拷贝。

目前，块设备驱动统一为跨架构 virtio-blk，`BlockDevice` 的派生类不再感知底层是 MMIO 还是 PCI 传输。配合这一变化，`BlockDeviceIoctlState` 从 `syscall_handler` 中独立抽离，集中管理 BLKRASET/BLKRAGET 等块设备专属 ioctl，使 ioctl 分发按设备类型拆分。

=== 流设备的高级功能

流设备（`StreamDevice`）作为字符设备的高级扩展，在逐字符读写的基础上增加了流重定向、批量数据传输等高级特性，使其更适合于复杂的数据流处理场景。

```cpp
class StreamDevice : public CharDevice
{
protected:
    CharDevice *_stream = nullptr;  // 底层字符设备
public:
    virtual bool support_stream() override { return true; }
    virtual long read(void *dst, long n_bytes, bool nonblocking) = 0;
    virtual long write(void *src, long n_bytes) = 0;
    int redirect_stream(CharDevice *dev);  // 流重定向
};
```

它的定位是 VFS 与字符硬件之间的适配层：上层 `device_file` 需要以字节流方式批量读写数据，而底层 `CharDevice` 只能逐字符收发。`StreamDevice` 填补了这个粒度差距——对外提供 `read` / `write` 两个批量接口供 VFS 调用，内部则循环调用 `get_char` / `put_char` 将字节流与逐字符操作相互转换。

流设备的一个重要特性是流重定向功能。通过 `redirect_stream` 方法，可将一个流设备的输出重定向到另一个字符设备，实现灵活的数据流路由。这一功能在系统的输入输出重定向、设备驱动程序的级联等场景中发挥着重要作用。


=== Loop 设备

Loop 设备的核心思想是：把文件当磁盘用——将一个普通文件包装成块设备，使得文件系统镜像（如 ISO 光盘映像、ext4 分区映像）可以直接挂载和访问，无需额外的物理磁盘或分区。

在结构上，Loop 设备分为控制面和数据面两部分。控制面是一个注册在 `/dev/loop-control` 的字符设备，负责 loop 实例的创建、销毁和查找。数据面是若干个 `/dev/loopN` 设备节点（默认 8 个，最多 256 个），每个节点绑定一个后端文件，将外部发来的块读写请求转发为该文件的普通读写。

使用时，用户打开一个普通文件，通过 ioctl 将其关联到某个空闲的 loop 设备上。loop 设备会独立持有该文件的引用，用户关闭原始文件描述符后设备仍可继续使用。此后，对该 loop 设备的块读写请求会加上配置的偏移量，映射到后端文件的对应位置，从而使得存储在文件中的文件系统能够像真实磁盘分区一样被挂载到目录树上。


== 块设备驱动与 I/O 调度

=== 统一 virtio-blk 框架



F7LY 的 virtio-blk 驱动采用统一的三层框架，将块设备 I/O 中与硬件传输相关的部分和与请求管理相关的部分清晰拆开，在双架构下维持一致的上层接口。


#figure(
  image("fig/块设备驱动与IO调度.png", width: 100%),
  caption: [块设备驱动与IO调度],
) <fig:fs-virtio-blk-architecture>

三层框架自底向上依次为传输层、队列层和设备层。传输层（`VirtioBlkTransport`）是唯一与架构相关的部分，封装了 DMA 地址翻译、队列通知和中断确认——RISC-V 走 MMIO，LoongArch 走 PCI，对上暴露完全一致的接口。队列层（`VirtioBlkQueue`）负责 virtqueue 描述符的分配与回收、请求的调度排序以及 in-flight 请求的完成回收，它的所有逻辑与传输方式无关。设备层（`VirtioBlkDevice`）将传输层和队列层聚合在一起，对上暴露 `submit_and_wait` 和 `submit_transfer_and_wait` 两个同步提交接口：前者走 buffer cache 路径，后者支持任意扇区范围的直接读写，ext4 文件系统只需要传入缓冲区、起始扇区和读写方向，阻塞等待完成即可。

三层之间的数据载体是统一的 `IoRequest` 结构，它携带扇区号、数据缓冲区、读写方向和提交进程的 PID 与 nice 值。nice 值随请求传入队列层的 mClock 调度器，由调度器决定请求的发出顺序，从而将进程优先级从 CPU 调度延伸到磁盘 I/O。


=== mClock 调度器

mClock 调度器位于 `VirtioBlkQueue` 的内部，处于 I/O 请求入队到写入 virtqueue 描述符之间的关键路径上，负责决定多个进程的并发 I/O 请求以何种顺序派发给磁盘硬件。

调度器首先将请求按提交进程的 nice 值映射为 8 个服务类——nice 每 5 级合为一类，class 0（最高优先级）对应 weight 256 和 24 MB/s 保底带宽，class 7（最低）对应 weight 8 和 256 KB/s 保底带宽。每类内部按 PID 区分 per-flow 队列，同类内的多个进程通过轮转指针 `rr_hint` 轮流获得服务，避免单进程霸占该类全部带宽。

mClock 算法的核心是为每个请求打上三个独立的时间标签：R-tag（Reservation）按保底带宽推进，保证每类至少获得 reservation 承诺的最低带宽；W-tag（Weight）按权重和当前磁盘吞吐的 EWMA 估计推进，权重越高 tag 推进越慢，在空闲带宽竞争中越占优势；L-tag（Limit）按上限带宽推进，当前所有类上限均为 unlimited。派发时，调度器扫描所有类中所有 flow 的头请求，优先选取 R-tag 已到期的请求，其次选 W-tag 最早者，L-tag 未到期的请求被暂时 gate 住并返回最早解禁时间。

$ R_i^r = max{ R_i^{r-1} + 1/r_i, t } $

$ L_i^r = max{ L_i^{r-1} + 1/l_i, t } $

$ P_i^r = max{ P_i^{r-1} + 1/w_i, t } $


调度器通过 EWMA 动态估计磁盘有效吞吐（初始 64 MB/s，每次完成时以 $alpha = 1/8$ 更新），使 W-tag 的计算自适应硬件实际能力。当仅有一个类活跃时，调度器走快速路径直接轮转，避免跨类扫描开销。mClock 的优势在于同时提供保底带宽、公平分享和上限控制三种约束，能够在较低实现复杂度下兼顾响应性与吞吐量。
#figure(
  image("fig/mClock.png", width: 100%),
  caption: [Reservation标签调整],
) <fig:fs-mclock-scheduler>

== 系统文件

VFS 在 F7LY 中承上启下。对上，所有文件相关的系统调用最终都落到 VFS 层的 `file` 对象上，用户态程序不感知底层文件系统是 ext4、FAT32 还是虚拟文件。对下，VFS 对接三类后端：ext4 主力根文件系统（通过 lwext4 库，经 buffer 层走 virtio-blk）、FAT32 辅助数据盘（内置实现，统一走 virtio-blk）、以及 `/proc`、`/dev`、`/etc` 等虚拟文件（由 `VirtualFileSystem` 的目录树节点挂载 `VirtualContentProvider` 按需生成内容）。核心枢纽是全局对象 `k_vfs`，提供统一的 `openat`、`fstat`、`is_file_exist` 等入口。

#figure(
  image("fig/vfs_structure.png", width: 100%),
  caption: [vfs_structure],
) <fig:fs-vfs-structure>

文件打开时，`k_vfs` 按三层优先级查找：先查虚拟文件树，命中则创建 `virtual_file`；其次匹配 `/dev/` 前缀，命中则通过 `DeviceManager` 创建 `device_file`；其余路径交由 ext4 或 FAT32 完成 inode 查找，创建 `normal_file` 或 `directory_file`。找到的资源被封装为对应的 `file` 子类对象，写入进程 fd 表后返回文件描述符给用户态。

所有文件类型均继承自抽象基类 `file`，override `read`、`write`、`read_ready`、`write_ready`、`lseek`、`read_sub_dir` 等接口，形成统一的文件对象体系。七种派生类及各自对接的后端如下：

- `normal_file`：普通文件，对接 ext4 / FAT32 磁盘文件。
- `directory_file`：目录，对接 ext4 / FAT32 目录项遍历。
- `device_file`：设备节点，对接 `DeviceManager`，按设备号路由到字符设备或块设备。
- `pipe_file`：管道，持有 `Pipe` 循环缓冲区，支持 FIFO 命名管道。
- `socket_file`：socket，对接 loopback 数据面或 ONPS 协议栈，支持 TCP / UDP / UNIX。
- `virtual_file`：虚拟文件，持有 `VirtualContentProvider`，read 时动态生成内容。
- `epoll_file`：epoll 实例，持有被关注 fd 列表，通过 `read_ready`/`write_ready` 回调统一轮询就绪状态。

== VFS 核心元数据

=== Buffer 层

Buffer 层是夹在文件系统与磁盘驱动之间的一层缓存。它的核心职责是：把磁盘块的读写结果留在内存里，下次访问同一块时直接命中，避免反复读盘；同时作为多个进程访问同一磁盘块的同步点，保证同一时刻只有一个进程在修改某个块的数据。F7LY 的 buffer 层由两部分组成——底层是经典的 `buf` 缓存（双向链表，按 LRU 排序，`bread` 读、`bwrite` 写、`brelse` 释放时移到链表头部），上层是 lwext4 内置的 `ext4_bcache`（红黑树按块号索引、LRU 队列淘汰、脏链表批量回写）。ext4 的块读写统一走 `ext4_bcache`，在缓存未命中时才穿透到底层的 `buf` 和 virtio-blk。

为提升 iozone 这类批量读写场景的性能，F7LY 在 buffer 层向下提交 I/O 的关键路径上引入了相邻块合并机制。ext4 以 4KB 为逻辑块大小，而磁盘按 512B 扇区寻址，一次 `read` 或 `write` 往往涉及多个连续的 4KB 块。`vfs_ext4_blockdev` 预先分配了一段 128KB 的连续物理页作为 DMA bounce buffer，`blockdev_rw_common` 在提交前将连续的多个 4KB 块聚合成一批（最多 32 块 = 128KB），统一通过一次 `virtio_disk_rw_sectors` 下发到 virtio-blk 队列。原来每个 4KB 块单独提交需要 32 次磁盘来回，合并后最多减少为 1 次，显著降低了 virtqueue 描述符分配、队列通知和中断回收的频次。此外，`normal_file` 内部还有一层 1MB 的 write-combine 缓冲区，将用户态频繁的小 `write` 系统调用在到达 ext4 之前先聚合，进一步降低 ext4 的事务和锁开销。

=== SuperBlock 和 Inode

超级块是文件系统的核心元数据结构，承担着存储文件系统全局配置信息的重要职责。每个超级块实例代表一个具体的文件系统对象——它记录了这个文件系统的块大小、总块数、根目录位置、魔数等基础参数，后续所有的块分配、inode 查找、空间统计都从这里出发。

针对持久化文件系统（如 ext4），其生命周期包含两个关键阶段：

- 挂载时，内核从磁盘的元数据区域读出原始超级块信息，在内存中构建运行时结构；
- 卸载时，将修改后的元数据同步回磁盘并释放内存中的超级块对象。

在 F7LY 中，这一过程由两层结构协作完成——VFS 层的通用 `superblock` 负责块大小、魔数、根 inode 指针、脏 inode 链表等文件系统无关的通用信息；lwext4 内部的 `ext4_sblock` 则以紧凑的 packed 格式存放 ext4 专属的磁盘布局细节（块组描述符分布、inode 位图位置、日志起止、特性标志位等），由 ext4 的块分配器和 inode 分配器直接使用。挂载时 ext4 驱动从磁盘读出 `ext4_sblock`，VFS 层则构建对应的通用 `superblock` 并挂入全局文件系统表，两层各司其职。

而对于非持久化的虚拟文件系统（如 `/proc`、`/dev`），F7LY 则完全跳过了超级块机制——它们不由 `superblock` 描述，而是直接由 `VirtualFileSystem` 维护一棵内存目录树，每个节点挂载一个 `VirtualContentProvider` 按需生成内容，没有挂载/卸载流程，也不涉及块分配或磁盘同步。这种差异化设计使得同一个 VFS 框架既能管理磁盘上的 ext4 和 FAT32，又能管理纯粹在内存中存在的信息节点。


```cpp
struct superblock {
    uint8   s_dev;              // 块设备标识符
    uint32  s_blocksize;        // 数据块大小（字节）
    uint32  s_magic;            // 文件系统魔数
    uint32  s_maxbytes;         // 最大文件大小
    struct inode *root;         // 根目录 inode 指针
    struct super_operations *s_op;  // 操作函数表
    SpinLock dirty_lock;
    struct list_head s_dirty_inodes; // 脏 inode 链表
};
```

`superblock` 的字段非常精简——仅保留所有文件系统共有的通用信息，共 9 个字段。设备标识符和块大小决定了 I/O 的基本粒度，魔数区分 ext4 与 FAT32，根 inode 指针是整个目录树的起点，脏 inode 链表则供 sync 操作找到需要写回的元数据。操作函数表 `super_operations` 当前为空，预留给未来的挂载/卸载/统计回调。

对于具体的文件系统，只需要移植自己的超级块结构体，并完善同样的函数实现。如 ext4 的超级块在 lwext4 中定义为紧凑的 packed 结构 `ext4_sblock`：

```cpp
#pragma pack(push, 1)
struct ext4_sblock {
 uint32_t inodes_count; /* I-nodes count */
    uint32_t blocks_count_lo; /* Blocks count */
    uint32_t reserved_blocks_count_lo; /* Reserved blocks count */
    uint32_t free_blocks_count_lo; /* Free blocks count */
    uint32_t free_inodes_count; /* Free inodes count */
    uint32_t first_data_block; /* First Data Block */
    uint32_t log_block_size; /* Block size */
    uint32_t log_cluster_size; /* Obsoleted fragment size */
    uint32_t blocks_per_group; /* Number of blocks per group */
    uint32_t frags_per_group; /* Obsoleted fragments per group */
    uint32_t inodes_per_group; /* Number of inodes per group */
    uint32_t mount_time; /* Mount time */
    uint32_t write_time; /* Write time */
    uint16_t mount_count; /* Mount count */
    uint16_t max_mount_count; /* Maximal mount count */
    uint16_t magic; /* Magic signature */
    uint16_t state; /* File system state */
    uint16_t errors; /* Behavior when detecting errors */
    uint16_t minor_rev_level; /* Minor revision level */
    uint32_t last_check_time; /* Time of last check */
    //……
};
#pragma pack(pop)
```

`ext4_sblock` 直接映射磁盘上超级块区域的字节布局，字段按 ext4 规范固定排列，`#pragma pack(1)` 确保没有编译器填充。挂载时 lwext4 从磁盘读出此结构，后续 ext4 的块分配器通过 `ext4_sb_get_blocks_cnt` 等内联函数读取其中字段；VFS 层则从中提取块大小、魔数等信息填入通用 `superblock`，两者各司其职。FAT32 同理，通过 `fat32_sblock` 存放 BPB 参数和 FSINFO 信息，实现同一套 VFS 接口。


*Inode* 是单个文件在 VFS 中的代表。无论文件来自 ext4、FAT32 还是管道，在 VFS 层都有一个对应的 `inode` 结构，它记录了除文件名之外的该文件全部静态属性：inode 编号、类型与权限、硬链接数、所有者、文件大小、占用块数、三个时间戳，以及指回所属超级块的指针。inode 的行为由操作函数表 `inode_operations` 定义——读取数据、写入数据、在目录中查找子文件、创建新文件、获取 stat 信息——这些操作在 ext4 和 FAT32 下的实现完全不同（ext4 走 extent tree 定位数据块，FAT32 沿 FAT 表追簇链），但 VFS 上层通过统一的函数表指针调用，不感知底层差异。每个 inode 内部嵌有两个文件系统专用的附属结构——一个存 ext4 文件路径，一个存 FAT32 簇链信息——用哪种文件系统就填哪种。

```cpp
struct inode_operations {
    void (*unlockput)(struct inode *self);
    void (*unlock)(struct inode *self);
    void (*put)(struct inode *self);
    void (*lock)(struct inode *self);
    void (*update)(struct inode *self);
    ssize_t (*read)(struct inode *self, int user_dst, uint64 dst, uint off, uint n);
    int (*write)(struct inode *self, int user_src, uint64 src, uint off, uint n);
    int (*isdir)(struct inode *self); // 是否是directory
    struct inode *(*dup)(struct inode *self);
    //For directory
    struct inode *(*dirlookup)(struct inode *self, const char *name, uint *poff);
    int (*deletei)(struct inode *self, struct inode *ip);            
    int (*dir_empty)(struct inode *self);
    struct inode *(*create)(struct inode *self, const char *name, uchar type, short major, short minor);
    void (*stat)(struct inode *self, struct stat *st);
};

extern struct inode_operations inode_ops;

#define EXT4_PATH_LONG_MAX 1024

struct vfs_ext4_inode_info {
    char fname[EXT4_PATH_LONG_MAX];
};

/*
 *索引节点
 */
struct inode {
    uint8 i_dev;
    uint16 i_mode; //类型 & 访问权限
    //……省略其他字段
    SpinLock lock; 
    struct inode_operations *i_op; //inode操作函数
    struct superblock *i_sb;
    struct vfs_ext4_inode_info i_info; //EXT4 inode结构
    struct vfs_fat32_inode_info i_fat_info;
};


```

== ext4 与 FAT32

F7LY 同时支持两套文件系统，各司其职。ext4 是主力根文件系统，承载 `/` 下的系统目录、用户程序、配置文件、动态链接器等全部内容，通过集成的 lwext4 库提供完整的 ext4 特性支持——扩展树（extent tree）管理数据块的索引，日志（journal）保证元数据一致性，目录哈希索引加速大目录的查找，扩展属性（xattr）支持文件级别的键值对存储。FAT32 则作为辅助数据盘挂载在 `/fat32` 下，由内核内置的轻量 FAT32 实现驱动，主要用于数据交换和兼容场景。

两套文件系统的 I/O 路径最终统一收口到统一 virtio-blk 框架。ext4 通过 `vfs_ext4_blockdev` 将 lwext4 的块读写请求映射为扇区级 I/O，FAT32 则通过内部的扇区读写函数直接调用 `virtio_disk_rw_sectors`，两者共享同一个 virtio-blk 队列和 mClock 调度器。因此对于底层磁盘驱动而言，ext4 和 FAT32 的请求没有区别——同样的传输层、同样的描述符队列、同样的调度策略。

在对上层的 VFS 接口上，ext4 和 FAT32 各自提供一套 `filesystem_op`（含 mount / umount / statfs / sync）和 `inode_operations`（含 read / write / create / dirlookup / stat），注册到全局文件系统表。路径解析到某个挂载点后，VFS 按该挂载点登记的文件系统类型分发到对应的操作函数。两套实现在内部差异显著——ext4 走 lwext4 的块缓存和 extent tree 定位数据，FAT32 沿 FAT 表的簇链逐簇追踪——但对外暴露的 `file` 对象和 `inode` 接口完全一致，用户态和系统调用层不感知区别。

== 文件操作

除了基本的 `read` 和 `write`，F7LY 还提供了几类更丰富的文件级操作。

- `fcntl`：文件描述符的通用控制接口。它负责多个功能——复制文件描述符（`F_DUPFD`）、管理 close-on-exec 标志（`F_GETFD`/`F_SETFD`）、读写文件状态标志如 O_NONBLOCK 和 O_APPEND（`F_GETFL`/`F_SETFL`）、对文件区间施加 POSIX 记录锁（`F_SETLK`/`F_SETLKW`/`F_GETLK`）、调整管道容量（`F_SETPIPE_SZ`）、以及对 memfd 施加写保护和缩减密封（`F_ADD_SEALS`）。本质上，`fcntl` 是文件描述符层面的“属性面板”，和具体读写路径分离。

- `ioctl`：设备的通用控制通道。不同于 `read`/`write` 传递的是数据流，`ioctl` 传递的是控制命令和参数——查询磁盘容量、调整预读扇区数、设置终端波特率、查询网络接口信息。ioctl 按设备类型拆分为独立处理模块：块设备、socket、终端各有自己的 ioctl 处理器。

- xattr（扩展属性）：文件上的键值对存储，独立于文件数据内容之外。通过 `setxattr`/`getxattr`/`listxattr`/`removexattr` 来读写。典型用途包括存储文件不可变标志、安全标签和 ACL 权限列表。F7LY 同时支持按路径和按 fd 两种访问方式。

- `splice`：两个文件描述符之间的零拷贝数据搬运。传统方式需要先把数据读到用户态再写回内核，`splice` 绕过了这个步骤，直接在内核中将一个文件描述符的缓冲区内数据搬到另一个——比如把文件内容直接送入 socket 发送区，省掉两次用户态拷贝。

- `fanotify`：文件访问监控。用户态程序创建一个 fanotify 实例并指定要监控的文件和事件类型（打开、关闭、修改、删除等），之后每当这些事件发生时，VFS 就在对应的操作路径（`openat`、`close`、`unlink` 等）上产生事件通知，监控程序通过 `read` 获取。这为文件系统审计和实时监控提供了基础设施。

== 挂载与路径解析

挂载系统负责将磁盘上的文件系统接入 VFS 的目录树，使得不同设备上的数据可以通过统一的路径空间来访问。F7LY 支持三种挂载方式：常规挂载（`mount`）将一个块设备上的文件系统绑定到某个目录；bind 挂载将一个已有目录镜像到另一个位置，使同一份数据可以通过两条路径访问；move 挂载则将已有挂载点整体迁移到新位置。卸载（`umount`）执行反向操作，将文件系统从目录树上剥离并释放相关资源。同时我们还有挂载传播类型的支持——挂载点可标记为私有（private）、共享（shared）、从属（slave）或不可绑定（unbindable），为容器场景下的挂载视图隔离提供了基础。

与挂载系统配合的是 mount namespace 机制。每个进程属于一个 mount namespace，拥有独立的挂载点视图——在一个 namespace 中执行的挂载或卸载不会影响其他 namespace。创建新 namespace 时，内核复制当前挂载视图并建立独立的引用关系；当最后一个持有者退出后，对应的挂载视图会自动销毁。

路径解析是每次文件访问的第一步，负责将用户态传入的路径字符串转换为 VFS 可操作的 inode。解析流程先做语法规范化，再按挂载视图逐层解析路径分量，区分虚拟路径、设备路径和磁盘路径，并将最终定位到的 inode 返回给调用方。
== 虚拟文件与特殊设备

F7LY 的 VFS 除了管理磁盘文件系统，还维护了一整套不依赖磁盘的虚拟文件与特殊设备。它们通过 `VirtualFileSystem` 的树形目录结构组织，在初始化时逐个注册，对外表现为普通的文件节点，但数据在读取时由对应的 `VirtualContentProvider` 动态生成，或者来自硬件设备。

`/proc` 是系统信息的动态窗口：

- `/proc/self/exe` — 当前进程可执行文件的符号链接
- `/proc/self/cmdline` — 当前进程的命令行参数
- `/proc/self/maps` — 当前进程的 VMA 内存映射
- `/proc/self/pagemap` — 当前进程页面映射
- `/proc/self/stat` — 当前进程状态统计
- `/proc/self/status` — 当前进程详细状态
- `/proc/self/ns/time`、`/proc/self/ns/time_for_children`、`/proc/self/timens_offsets` — 时间命名空间句柄
- `/proc/meminfo` — 系统内存使用统计
- `/proc/cpuinfo` — CPU 硬件信息
- `/proc/version` — 内核版本字符串
- `/proc/uptime` — 系统运行时间
- `/proc/stat` — 系统全局统计
- `/proc/interrupts` — 中断统计信息
- `/proc/mounts` / `/proc/self/mounts` — 文件系统挂载信息

此外，`/proc/<pid>` 路径通过 `dynamic_file_type` 动态识别为目录，`/proc/<pid>/ns/mnt` 则返回对应进程所属 mount namespace 的句柄，供 `setns` 系统调用使用。

`/proc/sys` 提供了一组可读写的内核参数：

- `/proc/sys/kernel/pid_max` — 最大进程 ID
- `/proc/sys/kernel/shmmax` — 共享内存段最大字节数
- `/proc/sys/kernel/shmmni` — 共享内存段最大数量
- `/proc/sys/kernel/shmall` — 共享内存总页数限制
- `/proc/sys/kernel/tainted` — 内核污染状态
- `/proc/sys/kernel/domainname` — 域名
- `/proc/sys/kernel/random/entropy_avail` — 随机熵可用量
- `/proc/sys/fs/pipe-user-pages-soft` / `pipe-max-size` — 管道限制
- `/proc/sys/fs/lease-break-time` — 文件 lease 超时
- `/proc/sys/fs/inotify/max_queued_events` / `max_user_instances` — inotify 限制
- `/proc/sys/net/ipv4/conf/default/tag` / `conf/lo/tag` — 网络标签
- `/proc/sysvipc/shm` — System V 共享内存信息

`/sys` 提供了 CPU 和 NUMA 节点的拓扑信息：`/sys/devices/system/cpu/online`、`/sys/devices/system/cpu/present`、`/sys/devices/system/cpu/possible` 等列出可用 CPU；`/sys/devices/system/node/online`、`/sys/devices/system/node/node0/cpulist`、`/sys/devices/system/node/node0/distance`、`/sys/devices/system/node/node0/meminfo` 等提供 NUMA 节点信息。

`/etc` 下的配置文件以内核内置数据的方式提供：

- `/etc/passwd`、`/etc/group` — 用户与组信息
- `/etc/hosts` — 主机名解析
- `/etc/ld.so.preload`、`/etc/ld.so.cache` — 动态链接器配置
- `/etc/resolv.conf` — DNS 解析器
- `/etc/protocols` — 协议号映射
- `/etc/localtime` — 时区信息

`/boot` 目录下提供内核配置文件节点：`/boot/config-6.17.0` 和 `/boot/config-5.15.0-F7LY`，供用户态工具检查内核编译选项。

`/dev` 目录下注册了各类设备节点：

- `/dev/null`、`/dev/zero`、`/dev/full` — 特殊数据设备
- `/dev/urandom` — 随机数设备
- `/dev/loop-control` — Loop 设备控制接口（字符设备）
- `/dev/loop0` ~ `/dev/loop7` — Loop 块设备
- `/dev/block/8:0` — 块设备节点
- `/dev/ptmx`、`/dev/pts/0` — 伪终端设备
- `/dev/net/tun`、`/dev/tun` — 网络隧道设备
- `/dev/rtc`、`/dev/rtc0`、`/dev/misc/rtc` — 实时时钟设备
- `/dev/cpu_dma_latency` — CPU DMA 延迟控制

这些虚拟文件与特殊设备的共同之处在于：它们在 VFS 中都表现为统一的 `file` 对象，用户态的 `open`、`read`、`write`、`close` 操作与操作磁盘文件完全一致，但数据来源可以是内存统计、内核参数、硬件寄存器、或者另一个文件。这正是 VFS 多态设计的价值所在：一条统一的文件接口，背后是差异巨大的实现。

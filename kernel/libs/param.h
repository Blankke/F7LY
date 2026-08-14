#pragma once

// CPU 静态容量由平台画像给出，避免架构公共层猜测具体机器拓扑。运行时仍只
// 上线 DTB 实际声明且完成本地初始化的 CPU。
#ifndef F7LY_MAX_CPUS
#error "platform profile must define F7LY_MAX_CPUS"
#endif
#if F7LY_MAX_CPUS < 1 || F7LY_MAX_CPUS > 64
#error "F7LY_MAX_CPUS must fit in the 64-bit CPU mask"
#endif
#define NCPU F7LY_MAX_CPUS  // maximum number of CPUs supported by this kernel image
#define NUMCPU        NCPU
// 进程池需要覆盖较大的并发进程/线程集合；容量过小会让合法 fork()/clone()
// 负载过早得到 EAGAIN，破坏用户态按 Linux 语义创建任务的预期。
#define NPROC       512  // maximum number of processes
// 并行构建器会同时持有大量源码、元数据、管道和目录句柄。per-process fd 表
// 的权威容量已是 proc::fd_table_capacity=1024；这里同步旧式 VFS 全局对象池，
// 避免系统在内存仍充足时过早误报 EMFILE/ENFILE。
#define NOFILE     1024  // legacy per-process open-file capacity
#define NFILE      1024  // open files/ext4 handles per system
#define NINODE     1024  // maximum number of active inodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       0  // device number of file system root disk
#define MAXARG      1024 // max exec argument/environment strings
#define MAXENV        8  // max exec environment
#define MAXOPBLOCKS  20  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
// 块缓存过浅会让小块并发读写在 buffer cache 与块层之间频繁抖动；
// 这里把缓存深度提升到更适合通用文件系统负载的量级。
#define NBUF         1024  // size of disk block cache
#define FSSIZE       2000  // size of file system in blocks
#define MAXPATH      260   // maximum file path name
#define VFS_MAX_FS   4     // VFS 中最多的fs个数
#define INTERVAL     (390000000 / 200)
#define TMPDEV 2    // NOTE 用于挂载的临时设备号

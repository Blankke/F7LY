#pragma once

// 决赛评测的 RV 画像使用 8 vCPU，LA 画像使用 12 vCPU。按架构保留准确的
// 静态容量，既让 LA 吃满评测资源，也不让 RV 的调度/TLB 热路径多扫描空槽位；
// 运行时仍只会上线 DTB 实际声明并完成本地初始化的 CPU。
#ifdef LOONGARCH
#define NCPU         12  // maximum number of CPUs supported by this kernel image
#else
#define NCPU          8  // maximum number of CPUs supported by this kernel image
#endif
#define NUMCPU        NCPU
// 进程池需要覆盖较大的并发进程/线程集合；容量过小会让合法 fork()/clone()
// 负载过早得到 EAGAIN，破坏用户态按 Linux 语义创建任务的预期。
#define NPROC       512  // maximum number of processes
// Cargo/rustc 的并行子进程会同时持有大量源码、metadata、pipe 和目录句柄。
// per-process fd 表的权威容量已是 proc::fd_table_capacity=1024；这里把旧式
// VFS 全局对象池同步提升，避免 8 jobs 在内存仍充足时先误报 EMFILE/ENFILE。
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

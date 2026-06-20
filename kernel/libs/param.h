#pragma once

#define NUMCPU 1
// 进程池需要覆盖较大的并发进程/线程集合；容量过小会让合法 fork()/clone()
// 负载过早得到 EAGAIN，破坏用户态按 Linux 语义创建任务的预期。
#define NPROC       512  // maximum number of processes
#define NCPU          1  // maximum number of CPUs
#define NOFILE      128  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       0  // device number of file system root disk
#define MAXARG       32  // max exec arguments
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

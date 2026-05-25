#pragma once

#define NUMCPU 1
#define NPROC        64  // maximum number of processes
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
// iozone 的 1KiB 并发读写会把 512B 块缓存打得非常碎，缓存过浅会导致
// buffer cache 与块层来回抖动。这里直接把缓存深度提升到工程上可用的量级。
#define NBUF         1024  // size of disk block cache
#define FSSIZE       2000  // size of file system in blocks
#define MAXPATH      260   // maximum file path name
#define VFS_MAX_FS   4     // VFS 中最多的fs个数
#define INTERVAL     (390000000 / 200)
#define TMPDEV 2    // NOTE 用于挂载的临时设备号

#pragma once
#include "fs/vfs/fs.hh"
#include "sleeplock.hh"
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  proc::SleepLock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  // 以下字段仅用于块层软件调度，不参与缓存寻址语义。
  int io_write;               // 0 读请求，1 写请求
  int io_service_class;       // mClock 服务类编号
  uint io_submit_pid;         // 提交该请求的进程 PID
  int io_submit_nice;         // 提交时的 nice 值
  uint64 io_enqueue_us;       // 入调度队列时间
  uint64 io_request_bytes;    // 请求大小，当前固定为一个块
  uint64 io_r_tag_us;         // reservation 标签
  uint64 io_w_tag_us;         // weight 标签
  uint64 io_l_tag_us;         // limit 标签
  struct buf *io_next;        // 软件调度队列链指针
  uchar data[BSIZE];
};

void            binit(void);
struct buf*     bget(uint dev, uint sectorno);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

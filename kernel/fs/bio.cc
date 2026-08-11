// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.hh"
#include "param.h"
#include "spinlock.hh"
#include "sleeplock.hh"
#include "hal/arch.hh"

#include "fs/vfs/fs.hh"
#include "fs/buf.hh"
#include "devs/device_manager.hh"
#include "devs/block_device.hh"
#include "fs/drivers/platform_block.hh"
struct {
  SpinLock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  bcache.lock.init("bcache");


  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    b->lock.init("buffer lock", "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  bcache.lock.acquire();

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      bcache.lock.release();
      b->lock.acquire();
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      bcache.lock.release();
      b->lock.acquire();
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    // ROOTDEV 是平台块层发布的逻辑根分区；其它设备号必须在设备表中明确
    // 注册为块设备。不能用“查找失败就回退根盘”，否则错误设备号会静默
    // 读到另一块盘的同一 blockno。
    if (dev == ROOTDEV) {
      platform_block_rw(b, 0);
    } else {
      dev::VirtualDevice *vdev = dev::k_devm.get_device(dev);
      if (vdev == nullptr || vdev->type() != dev::DeviceType::dev_block) {
        panic("bread: unknown block device dev=%u", dev);
      }
      dev::BlockDevice *bd = static_cast<dev::BlockDevice *>(vdev);
      dev::BufferDescriptor bsecs[1];
      bsecs[0].buf_addr = (uint64)b->data;
      bsecs[0].buf_size = BSIZE;
      if (bd->read_blocks(blockno, 1, bsecs, 1) != 0) {
        // 已经定位到具体设备后，I/O 失败不能再落到平台根盘，否则会把
        // “设备故障”错误地转换成“从另一块盘读取同一块号”。
        panic("bread: block device read failed dev=%u block=%u", dev, blockno);
      }
    }

    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!(b->lock.is_holding()))
    panic("bwrite");

  if (b->dev == ROOTDEV) {
    platform_block_rw(b, 1);
  } else {
    dev::VirtualDevice *vdev = dev::k_devm.get_device(b->dev);
    if (vdev == nullptr || vdev->type() != dev::DeviceType::dev_block) {
      panic("bwrite: unknown block device dev=%u", b->dev);
    }
    dev::BlockDevice *bd = static_cast<dev::BlockDevice *>(vdev);
    dev::BufferDescriptor bsecs[1];
    bsecs[0].buf_addr = (uint64)b->data;
    bsecs[0].buf_size = BSIZE;
    if (bd->write_blocks(b->blockno, 1, bsecs, 1) != 0) {
      panic("bwrite: block device write failed dev=%u block=%u",
            b->dev, b->blockno);
    }
  }
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!(b->lock.is_holding()))
    panic("brelse");

  b->lock.release();

  bcache.lock.acquire();
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  bcache.lock.release();
}

void
bpin(struct buf *b) {
  bcache.lock.acquire();
  b->refcnt++;
  bcache.lock.release();
}

void
bunpin(struct buf *b) {
  bcache.lock.acquire();
  b->refcnt--;
  bcache.lock.release();
}

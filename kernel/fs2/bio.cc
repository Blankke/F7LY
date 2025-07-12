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
#include "platform.hh"

#include "fs2/vfs/fs.hh"
#include "fs2/buf.hh"
#ifdef RISCV
#include "drivers/riscv/virtio2.hh"
#elif defined LOONGARCH
#include "drivers/loongarch/virtio2.hh"
#endif
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
    if (dev == 0) {
      virtio_disk_rw(b, 0);
    } else {
      virtio_disk_rw2(b, 0);
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
  if (b->dev == 0) {
    virtio_disk_rw(b, 1);
  } else {
    virtio_disk_rw2(b, 1);
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



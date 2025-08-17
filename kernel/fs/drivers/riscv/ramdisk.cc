#include "fs/buf.hh"
#include "printer.hh"
#include "spinlock.hh"
#include "libs/string.hh"
#include "types.hh"

#define NRAMDISKPAGES (sddata_size * BSIZE / PGSIZE)

SpinLock ramdisklock;
extern uchar sddata_start[];
extern uchar sddata_end[];
char *ramdisk;

void ramdisk_init(void) {
  ramdisklock.init( "ramdisk lock");
  ramdisk = (char *)sddata_start;
  printf("ramdiskinit ram start:%p\n", ramdisk);
}

void ramdisk_read(struct buf *b) {
  ramdisklock.acquire();
  uint blockno = b->blockno;

  char *addr = ramdisk + blockno * BSIZE;
  memmove(b->data, (void *)addr, BSIZE);
  ramdisklock.release();
}

void ramdisk_write(struct buf *b) {
  ramdisklock.acquire();
  uint blockno = b->blockno;

  char *addr = ramdisk + blockno * BSIZE;
  memmove((void *)addr, b->data, BSIZE);
  ramdisklock.release();
}
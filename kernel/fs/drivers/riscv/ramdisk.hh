#ifndef RAMDISK_H
#define RAMDISK_H
#include "fs/buf.hh"

void ramdisk_init(void);
void ramdisk_read(struct buf *b);
void ramdisk_write(struct buf *b);

#endif
#ifndef __DISK_H
#define __DISK_H

#include "fs/buf.hh"

void disk_init(void);
void disk_rw(buf *buf, bool write);
int disk_rw_sectors(int dev, void *buf, uint64 start_sector,
                    uint32 sector_count, bool write);
#ifdef VISIONFIVE2
void disk_set_partition_offset(int dev, uint64 start_sector);
#endif
void disk_intr();

#endif

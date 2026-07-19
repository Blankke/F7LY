#ifndef __DISK_H
#define __DISK_H

#include "fs/buf.hh"
void disk_init(void);                            // 初始化
void disk_rw(buf *buf, bool write);             // 对单个 512B 扇区缓存读写
int disk_rw_sectors(int dev, void *buf, uint64 start_sector,
                    uint32 sector_count, bool write); // 对连续扇区读写
void disk_intr();                                // VIO中断处理

#endif

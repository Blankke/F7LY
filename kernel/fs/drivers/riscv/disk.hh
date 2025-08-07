#ifndef __DISK_H
#define __DISK_H

#include "fs/buf.hh"
void disk_init(void);                            // 初始化
void disk_rw(buf* buf, bool write);            // 对磁盘的读写操作
void disk_intr();                                // VIO中断处理

#endif

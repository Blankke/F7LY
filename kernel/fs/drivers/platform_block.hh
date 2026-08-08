#pragma once

#include "types.hh"

struct buf;

// 架构公共文件只依赖这一层。板级实现负责控制器选择、根分区映射和容量边界。
bool platform_block_init();
void platform_block_rw(struct buf *buffer, int write);
int platform_block_rw_sectors(int dev, void *buffer, uint64 start_sector,
                              uint32 sector_count, int write);
uint64 platform_block_capacity_bytes(int dev);

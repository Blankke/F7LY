#pragma once

#include "types.hh"

struct buf;

// 架构公共文件只依赖这一层。板级后端只提供裸盘，根分区识别、逻辑映射和
// 容量边界全部由 platform_block.cc 统一负责。
bool platform_block_init();
void platform_block_rw(struct buf *buffer, int write);
int platform_block_rw_sectors(int dev, void *buffer, uint64 start_sector,
                              uint32 sector_count, int write);
uint64 platform_block_capacity_bytes(int dev);

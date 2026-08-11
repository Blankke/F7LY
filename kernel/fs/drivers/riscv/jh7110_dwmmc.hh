#pragma once

#include "types.hh"

namespace riscv::jh7110::dwmmc
{
/**
 * @brief 初始化 VisionFive2 上的 JH7110 DWMMC 控制器与主 SD 卡。
 *
 * 驱动只建立一张裸 512 字节扇区设备。MBR/GPT/ext4 的识别统一留给
 * platform_block，避免控制器驱动再保存一份分区偏移。
 */
bool initialize();

/**
 * @brief 同步读写连续的 512 字节裸扇区。
 * @return 成功返回 0，参数、边界或硬件错误返回 -1。
 */
int transfer(void *buffer, uint64 start_sector, uint32 sector_count, bool write);

/** @brief 返回从卡 CSD 解析出的容量，单位为字节。 */
uint64 capacity_bytes();
} // namespace riscv::jh7110::dwmmc

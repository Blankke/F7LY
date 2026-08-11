#pragma once

#include "types.hh"

namespace platform::block_backend
{
    // 当前公共块层的 sector 参数统一表示 512 字节逻辑扇区。VirtIO-blk
    // capacity 天然使用该单位，现有 2K1000 AHCI 路径也只发布 512 字节 LBA。
    // 若未来接入不同逻辑扇区大小的设备，应整体升级为显式 Geometry 契约。
    inline constexpr uint32 k_sector_size_bytes = 512;

    // 平台在编译期只选择并链接一个实现。这里仅描述“裸块设备”能力，
    // 分区识别与逻辑根盘映射由 fs/drivers/platform_block.cc 统一负责。
    bool initialize();
    int read_write(int dev, void *buffer, uint64 start_sector,
                   uint32 sector_count, bool write);
    uint64 capacity_bytes(int dev);
    const char *name();
} // namespace platform::block_backend

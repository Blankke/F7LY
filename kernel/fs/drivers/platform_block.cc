#include "platform_block.hh"

#include "fs/buf.hh"
#include "platform/block_backend.hh"
#include "printer.hh"

namespace
{
    constexpr uint32 k_sector_size = platform::block_backend::k_sector_size_bytes;
    constexpr uint32 k_primary_device = 0;
    constexpr uint32 k_max_gpt_entries_to_scan = 128;

    enum class RootLayout
    {
        unknown,
        whole_disk,
        raw,
        mbr,
        gpt,
    };

    uint64 g_root_start_sector = 0;
    uint64 g_root_sector_count = 0;
    RootLayout g_root_layout = RootLayout::unknown;
    uint8 g_partition_buffer[1024];

    constexpr const char *layout_name(RootLayout layout)
    {
        switch (layout)
        {
        case RootLayout::whole_disk:
            return "whole-disk";
        case RootLayout::raw:
            return "raw-ext4";
        case RootLayout::mbr:
            return "MBR";
        case RootLayout::gpt:
            return "GPT";
        default:
            return "unknown";
        }
    }

    constexpr uint32 read_le32(const uint8 *data)
    {
        return static_cast<uint32>(data[0]) |
               (static_cast<uint32>(data[1]) << 8) |
               (static_cast<uint32>(data[2]) << 16) |
               (static_cast<uint32>(data[3]) << 24);
    }

    uint64 read_le64(const uint8 *data)
    {
        return static_cast<uint64>(read_le32(data)) |
               (static_cast<uint64>(read_le32(data + 4)) << 32);
    }

    uint64 raw_sector_count(int dev)
    {
        return platform::block_backend::capacity_bytes(dev) / k_sector_size;
    }

    bool range_is_valid(uint64 start_sector, uint32 sector_count, uint64 capacity_sectors)
    {
        return sector_count != 0 && start_sector < capacity_sectors &&
               static_cast<uint64>(sector_count) <= capacity_sectors - start_sector;
    }

    bool raw_read(void *buffer, uint64 sector, uint32 count)
    {
        if (buffer == nullptr || !range_is_valid(sector, count, raw_sector_count(k_primary_device)))
        {
            return false;
        }
        return platform::block_backend::read_write(
                   k_primary_device, buffer, sector, count, false) == 0;
    }

    bool has_ext4_superblock(uint64 start_sector, uint64 partition_sectors)
    {
        // ext4 超级块从分区内偏移 1024 字节开始，magic 位于其中偏移 56。
        // 少于三个扇区的区域不可能完整包含这里要读取的超级块字段。
        if (partition_sectors < 3 || start_sector > UINT64_MAX - 2)
        {
            return false;
        }
        if (!raw_read(g_partition_buffer, start_sector + 2, 1))
        {
            return false;
        }
        return g_partition_buffer[56] == 0x53 && g_partition_buffer[57] == 0xef;
    }

    bool select_root(uint64 start_sector, uint64 sector_count, RootLayout layout)
    {
        const uint64 disk_sectors = raw_sector_count(k_primary_device);
        if (sector_count == 0 || start_sector >= disk_sectors ||
            sector_count > disk_sectors - start_sector ||
            !has_ext4_superblock(start_sector, sector_count))
        {
            return false;
        }
        g_root_start_sector = start_sector;
        g_root_sector_count = sector_count;
        g_root_layout = layout;
        return true;
    }

    bool scan_gpt()
    {
        if (!raw_read(g_partition_buffer, 1, 1))
        {
            return false;
        }
        static constexpr uint8 k_signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
        for (uint32 index = 0; index < sizeof(k_signature); ++index)
        {
            if (g_partition_buffer[index] != k_signature[index])
            {
                return false;
            }
        }

        const uint64 entries_lba = read_le64(g_partition_buffer + 72);
        uint32 entry_count = read_le32(g_partition_buffer + 80);
        const uint32 entry_size = read_le32(g_partition_buffer + 84);
        const uint64 disk_sectors = raw_sector_count(k_primary_device);
        if (entries_lba == 0 || entries_lba >= disk_sectors ||
            entry_size < 128 || entry_size > k_sector_size)
        {
            return false;
        }
        if (entry_count > k_max_gpt_entries_to_scan)
        {
            entry_count = k_max_gpt_entries_to_scan;
        }

        for (uint32 index = 0; index < entry_count; ++index)
        {
            const uint64 byte_offset = static_cast<uint64>(index) * entry_size;
            const uint64 sector_offset = byte_offset / k_sector_size;
            if (sector_offset > disk_sectors - entries_lba)
            {
                return false;
            }
            const uint64 sector = entries_lba + sector_offset;
            const uint32 in_sector = static_cast<uint32>(byte_offset % k_sector_size);
            const uint32 needed = in_sector + 48 > k_sector_size ? 2 : 1;
            if (!raw_read(g_partition_buffer, sector, needed))
            {
                return false;
            }
            const uint8 *entry = g_partition_buffer + in_sector;
            bool type_guid_present = false;
            for (uint32 byte = 0; byte < 16; ++byte)
            {
                type_guid_present |= entry[byte] != 0;
            }
            if (!type_guid_present)
            {
                continue;
            }
            const uint64 first = read_le64(entry + 32);
            const uint64 last = read_le64(entry + 40);
            // last-first+1 仅在 last>=first 时计算，避免损坏的 GPT 条目下溢。
            if (last >= first && select_root(first, last - first + 1, RootLayout::gpt))
            {
                return true;
            }
        }
        return false;
    }

    bool discover_root_partition()
    {
        const uint64 disk_sectors = raw_sector_count(k_primary_device);
        if (disk_sectors == 0)
        {
            return false;
        }
        // 裸 ext4 镜像是 QEMU 评测盘的常见形式，必须先于分区表探测。
        if (select_root(0, disk_sectors, RootLayout::raw))
        {
            return true;
        }
        if (!raw_read(g_partition_buffer, 0, 1) ||
            g_partition_buffer[510] != 0x55 || g_partition_buffer[511] != 0xaa)
        {
            return false;
        }

        bool protective_gpt = false;
        uint8 partition_types[4] = {};
        uint64 partition_starts[4] = {};
        uint64 partition_counts[4] = {};
        for (uint32 index = 0; index < 4; ++index)
        {
            const uint8 *entry = g_partition_buffer + 446 + index * 16;
            partition_types[index] = entry[4];
            partition_starts[index] = read_le32(entry + 8);
            partition_counts[index] = read_le32(entry + 12);
            protective_gpt |= partition_types[index] == 0xee;
        }
        for (uint32 index = 0; index < 4; ++index)
        {
            if (partition_types[index] != 0 && partition_types[index] != 0xee &&
                select_root(partition_starts[index], partition_counts[index], RootLayout::mbr))
            {
                return true;
            }
        }
        return protective_gpt && scan_gpt();
    }
} // namespace

bool platform_block_init()
{
    g_root_start_sector = 0;
    g_root_sector_count = 0;
    g_root_layout = RootLayout::unknown;

    if (!platform::block_backend::initialize())
    {
        platformDiagnosticError("[block] backend=%s initialization failed\n",
                                platform::block_backend::name());
        return false;
    }

    const uint64 disk_sectors = raw_sector_count(k_primary_device);
    if (disk_sectors == 0)
    {
        platformDiagnosticError("[block] backend=%s reported zero capacity\n",
                                platform::block_backend::name());
        return false;
    }
    if (!discover_root_partition())
    {
        // 没有 ext4 不是块设备初始化失败。把整盘显式交给文件系统策略，
        // 让它仍可识别 raw FAT，或使用 DTB initrd 作为 ext4 根。这里不能
        // 提前 panic，否则上层已经实现的 initrd 回退永远不可达。
        g_root_start_sector = 0;
        g_root_sector_count = disk_sectors;
        g_root_layout = RootLayout::whole_disk;
        platformDiagnosticWarn("[block] backend=%s has no ext4 region; exposing whole disk\n",
                               platform::block_backend::name());
    }

    platformDiagnosticInfo("[block] backend=%s dev=0 layout=%s start=%lu sectors=%lu capacity=%lu bytes\n",
                           platform::block_backend::name(), layout_name(g_root_layout),
                           g_root_start_sector, g_root_sector_count,
                           g_root_sector_count * static_cast<uint64>(k_sector_size));
    return true;
}

int platform_block_rw_sectors(int dev, void *buffer, uint64 start_sector,
                              uint32 sector_count, int write)
{
    if (sector_count == 0)
    {
        return 0;
    }
    if (buffer == nullptr || dev < 0)
    {
        return -1;
    }

    // 设备 0 暴露探测出的 ext4 区域；若未找到，则按上层启动策略要求暴露
    // 整盘。其他平台裸盘编号仍由当前唯一 backend 定义并执行相同边界检查。
    const uint64 logical_sector_count =
        dev == static_cast<int>(k_primary_device) ? g_root_sector_count : raw_sector_count(dev);
    if (!range_is_valid(start_sector, sector_count, logical_sector_count))
    {
        return -1;
    }

    const uint64 physical_sector =
        dev == static_cast<int>(k_primary_device) ? g_root_start_sector + start_sector : start_sector;
    return platform::block_backend::read_write(
        dev, buffer, physical_sector, sector_count, write != 0);
}

void platform_block_rw(struct buf *buffer, int write)
{
    if (buffer == nullptr ||
        platform_block_rw_sectors(buffer->dev, buffer->data, buffer->blockno, 1, write) != 0)
    {
        panic("platform block I/O failed");
    }
}

uint64 platform_block_capacity_bytes(int dev)
{
    if (dev < 0)
    {
        return 0;
    }
    if (dev == static_cast<int>(k_primary_device))
    {
        return g_root_sector_count * static_cast<uint64>(k_sector_size);
    }
    return raw_sector_count(dev) * static_cast<uint64>(k_sector_size);
}

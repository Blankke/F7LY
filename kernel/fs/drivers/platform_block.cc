#include "platform_block.hh"

#include "fs/buf.hh"
#include "fs/drivers/virtio_blk.hh"
#include "printer.hh"

#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
#include "fs/drivers/loongarch/ls2k1000_ahci.hh"
#endif

namespace
{
    constexpr uint32 k_sector_size = 512;
    uint64 g_root_start_sector = 0;
    uint64 g_root_sector_count = 0;

#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    uint8 g_partition_buffer[1024];

    uint32 read_le32(const uint8 *data)
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

    bool raw_read(void *buffer, uint64 sector, uint32 count)
    {
        return loongarch::ls2k1000::ahci::transfer(buffer, sector, count, false) == 0;
    }

    bool has_ext4_superblock(uint64 start_sector)
    {
        // ext4 超级块从分区内偏移 1024 字节开始，magic 位于其中偏移 56。
        if (!raw_read(g_partition_buffer, start_sector + 2, 1))
        {
            return false;
        }
        return g_partition_buffer[56] == 0x53 && g_partition_buffer[57] == 0xef;
    }

    bool select_root(uint64 start_sector, uint64 sector_count, const char *table, uint32 index)
    {
        const uint64 disk_sectors = loongarch::ls2k1000::ahci::capacity_bytes() / k_sector_size;
        if (sector_count == 0 || start_sector >= disk_sectors ||
            sector_count > disk_sectors - start_sector || !has_ext4_superblock(start_sector))
        {
            return false;
        }
        g_root_start_sector = start_sector;
        g_root_sector_count = sector_count;
        printfGreen("[block] root=%s partition%u start=%lu sectors=%lu\n",
                    table, index, start_sector, sector_count);
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
        if (entries_lba == 0 || entry_size < 128 || entry_size > k_sector_size)
        {
            return false;
        }
        if (entry_count > 128)
        {
            entry_count = 128;
        }

        for (uint32 index = 0; index < entry_count; ++index)
        {
            const uint64 byte_offset = static_cast<uint64>(index) * entry_size;
            const uint64 sector = entries_lba + byte_offset / k_sector_size;
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
            if (last >= first && select_root(first, last - first + 1, "GPT", index + 1))
            {
                return true;
            }
        }
        return false;
    }

    bool discover_root_partition()
    {
        const uint64 disk_sectors = loongarch::ls2k1000::ahci::capacity_bytes() / k_sector_size;
        if (disk_sectors == 0)
        {
            return false;
        }
        if (has_ext4_superblock(0))
        {
            g_root_start_sector = 0;
            g_root_sector_count = disk_sectors;
            printfGreen("[block] SATA disk contains a raw ext4 root\n");
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
                select_root(partition_starts[index], partition_counts[index], "MBR", index + 1))
            {
                return true;
            }
        }
        return protective_gpt && scan_gpt();
    }
#endif
} // namespace

bool platform_block_init()
{
    g_root_start_sector = 0;
    g_root_sector_count = 0;
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    if (!loongarch::ls2k1000::ahci::initialize())
    {
        return false;
    }
    if (!discover_root_partition())
    {
        printfRed("[block] SATA disk has no usable ext4 root partition\n");
        return false;
    }
    return true;
#elif defined(LOONGARCH)
    virtio_probe();
    virtio_disk_init();
    g_root_sector_count = virtio_disk_capacity_bytes(0) / k_sector_size;
    return g_root_sector_count != 0;
#else
    // RISC-V 的现有启动路径仍负责初始化 VirtIO；本门面只统一后续 I/O 调用。
    g_root_sector_count = virtio_disk_capacity_bytes(0) / k_sector_size;
    return g_root_sector_count != 0;
#endif
}

int platform_block_rw_sectors(int dev, void *buffer, uint64 start_sector,
                              uint32 sector_count, int write)
{
    if (sector_count == 0)
    {
        return 0;
    }
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    if (dev != 0 || start_sector >= g_root_sector_count ||
        static_cast<uint64>(sector_count) > g_root_sector_count - start_sector)
    {
        return -1;
    }
    return loongarch::ls2k1000::ahci::transfer(
        buffer, g_root_start_sector + start_sector, sector_count, write != 0);
#else
    return virtio_disk_rw_sectors(dev, buffer, start_sector, sector_count, write);
#endif
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
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    if (dev != 0)
    {
        return 0;
    }
    return g_root_sector_count * static_cast<uint64>(k_sector_size);
#else
    return virtio_disk_capacity_bytes(dev);
#endif
}

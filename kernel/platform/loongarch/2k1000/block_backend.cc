#include "platform/block_backend.hh"

#include "fs/drivers/loongarch/ls2k1000_ahci.hh"

namespace platform::block_backend
{
    bool initialize()
    {
        return loongarch::ls2k1000::ahci::initialize();
    }

    int read_write(int dev, void *buffer, uint64 start_sector,
                   uint32 sector_count, bool write)
    {
        if (dev != 0)
        {
            return -1;
        }
        return loongarch::ls2k1000::ahci::transfer(
            buffer, start_sector, sector_count, write);
    }

    uint64 capacity_bytes(int dev)
    {
        return dev == 0 ? loongarch::ls2k1000::ahci::capacity_bytes() : 0;
    }

    const char *name()
    {
        return "ls2k1000-ahci";
    }
} // namespace platform::block_backend

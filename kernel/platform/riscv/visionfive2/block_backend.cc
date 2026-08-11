#include "platform/block_backend.hh"

#include "fs/drivers/riscv/jh7110_dwmmc.hh"

namespace platform::block_backend
{
bool initialize()
{
    return riscv::jh7110::dwmmc::initialize();
}

int read_write(int dev, void *buffer, uint64 start_sector,
               uint32 sector_count, bool write)
{
    if (dev != 0)
    {
        return -1;
    }
    return riscv::jh7110::dwmmc::transfer(
        buffer, start_sector, sector_count, write);
}

uint64 capacity_bytes(int dev)
{
    return dev == 0 ? riscv::jh7110::dwmmc::capacity_bytes() : 0;
}

const char *name()
{
    return "visionfive2-jh7110-dwmmc";
}
} // namespace platform::block_backend

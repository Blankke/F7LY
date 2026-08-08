#pragma once

#include "types.hh"

namespace loongarch::ls2k1000::ahci
{
    bool initialize();
    int transfer(void *buffer, uint64 start_sector, uint32 sector_count, bool write);
    uint64 capacity_bytes();
}

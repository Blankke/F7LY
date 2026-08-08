#pragma once

#include "types.hh"

namespace loongarch::platform_irq
{
    void init();
    uint64 claim();
    void complete(uint64 sources);
    uint64 uart_source_mask();
    uint64 block_source_mask();
}

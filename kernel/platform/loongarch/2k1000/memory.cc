#include "platform/memory.hh"

#include "hal/loongarch/platform_board.hh"

namespace platform::memory
{
uint64 physical_address(uint64 address)
{
    return loongarch::board::physical_address(address);
}

uint64 kernel_access_address(uint64 address)
{
    return loongarch::board::cached_address(address);
}

uint64 managed_physical_top()
{
    return k_unlimited_physical_top;
}
} // namespace platform::memory

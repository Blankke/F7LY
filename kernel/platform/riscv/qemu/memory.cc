#include "platform/memory.hh"

namespace platform::memory
{
uint64 physical_address(uint64 address)
{
    return address;
}

uint64 kernel_access_address(uint64 address)
{
    return address;
}

uint64 managed_physical_top()
{
    return k_unlimited_physical_top;
}
} // namespace platform::memory

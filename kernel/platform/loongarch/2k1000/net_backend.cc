#include "net/drivers/net_backend.hh"

#include "net/drivers/loongarch/ls2k1000_gmac.hh"

namespace net::backend
{
bool initialize()
{
    return loongarch::ls2k1000::gmac::initialize();
}

int send(const void *data, uint32 length)
{
    return loongarch::ls2k1000::gmac::send(data, length);
}

int receive(void *data, uint32 *length)
{
    return loongarch::ls2k1000::gmac::receive(data, length);
}

void poll()
{
    loongarch::ls2k1000::gmac::poll();
}

void get_mac(uint8 mac[platform_device::k_mac_address_length])
{
    loongarch::ls2k1000::gmac::get_mac(mac);
}

const char *name()
{
    return "gmac0";
}

void debug_status()
{
    loongarch::ls2k1000::gmac::debug_status();
}
} // namespace net::backend

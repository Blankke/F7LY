#include "platform_net_device.hh"
#include "net_backend.hh"

namespace net::platform_device
{
bool initialize()
{
    return backend::initialize();
}

int send(const void *data, uint32 length)
{
    return backend::send(data, length);
}

int receive(void *data, uint32 *length)
{
    return backend::receive(data, length);
}

void poll()
{
    backend::poll();
}

void get_mac(uint8 mac[k_mac_address_length])
{
    backend::get_mac(mac);
}

const char *name()
{
    return backend::name();
}

void debug_status()
{
    backend::debug_status();
}
} // namespace net::platform_device

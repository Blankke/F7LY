#include "net/drivers/net_backend.hh"

#include "net/drivers/virtio_net.hh"

namespace net::backend
{
bool initialize()
{
    return virtio_net_init();
}

int send(const void *data, uint32 length)
{
    return virtio_net_send(data, length);
}

int receive(void *data, uint32 *length)
{
    return virtio_net_recv(data, length);
}

void poll()
{
    virtio_net_poll();
}

void get_mac(uint8 mac[platform_device::k_mac_address_length])
{
    virtio_net_get_mac(mac);
}

const char *name()
{
    return "virtio0";
}

void debug_status()
{
    virtio_net_debug_status();
}
} // namespace net::backend

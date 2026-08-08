#include "platform_net_device.hh"

#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
#include "loongarch/ls2k1000_gmac.hh"
#else
#include "virtio_net.hh"
#endif

namespace net::platform_device
{
bool initialize()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return loongarch::ls2k1000::gmac::initialize();
#else
    return virtio_net_init();
#endif
}

int send(const void *data, uint32 length)
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return loongarch::ls2k1000::gmac::send(data, length);
#else
    return virtio_net_send(data, length);
#endif
}

int receive(void *data, uint32 *length)
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return loongarch::ls2k1000::gmac::receive(data, length);
#else
    return virtio_net_recv(data, length);
#endif
}

void poll()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    loongarch::ls2k1000::gmac::poll();
#else
    virtio_net_poll();
#endif
}

void get_mac(uint8 mac[k_mac_address_length])
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    loongarch::ls2k1000::gmac::get_mac(mac);
#else
    virtio_net_get_mac(mac);
#endif
}

const char *name()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return "gmac0";
#else
    return "virtio0";
#endif
}

const char *ipv4_address()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return LS2K1000_IPV4;
#else
    return "10.0.2.15";
#endif
}

const char *ipv4_netmask()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return LS2K1000_NETMASK;
#else
    return "255.255.255.0";
#endif
}

const char *ipv4_gateway()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return LS2K1000_GATEWAY;
#else
    return "10.0.2.2";
#endif
}

const char *ipv4_dns()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return LS2K1000_DNS;
#else
    return "10.0.2.3";
#endif
}

const char *ipv4_broadcast()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    return LS2K1000_BROADCAST;
#else
    return "10.0.2.255";
#endif
}

void debug_status()
{
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
    loongarch::ls2k1000::gmac::debug_status();
#else
    virtio_net_debug_status();
#endif
}
} // namespace net::platform_device

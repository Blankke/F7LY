#include "net/network_config.hh"

#if !defined(LS2K1000_IPV4) || !defined(LS2K1000_NETMASK) || \
    !defined(LS2K1000_GATEWAY) || !defined(LS2K1000_DNS) ||   \
    !defined(LS2K1000_BROADCAST)
#error "LS2K1000 network configuration is missing"
#endif

namespace
{
// 2K1000 实板所在网段由构建参数决定；这些值属于部署配置，而不是 GMAC
// 驱动属性，因而集中保存在平台网络配置中。
constexpr net::NetworkConfig k_default_network_config{
    .ipv4_address = LS2K1000_IPV4,
    .ipv4_netmask = LS2K1000_NETMASK,
    .ipv4_gateway = LS2K1000_GATEWAY,
    .ipv4_dns = LS2K1000_DNS,
    .ipv4_broadcast = LS2K1000_BROADCAST,
};
} // namespace

namespace net
{
const NetworkConfig &default_network_config()
{
    return k_default_network_config;
}
} // namespace net

/* SPDX-License-Identifier: GPL-3.0-only */
#include "net/network_config.hh"

#if !defined(VISIONFIVE2_IPV4) || !defined(VISIONFIVE2_NETMASK) || \
    !defined(VISIONFIVE2_GATEWAY) || !defined(VISIONFIVE2_DNS) ||   \
    !defined(VISIONFIVE2_BROADCAST)
#error "VisionFive 2 network configuration is missing"
#endif

namespace
{
// IP 参数是部署配置，不属于 JH7110 GMAC 寄存器/PHY 驱动。
constexpr net::NetworkConfig k_default_network_config{
    .ipv4_address = VISIONFIVE2_IPV4,
    .ipv4_netmask = VISIONFIVE2_NETMASK,
    .ipv4_gateway = VISIONFIVE2_GATEWAY,
    .ipv4_dns = VISIONFIVE2_DNS,
    .ipv4_broadcast = VISIONFIVE2_BROADCAST,
};
} // namespace

namespace net
{
const NetworkConfig &default_network_config()
{
    return k_default_network_config;
}
} // namespace net

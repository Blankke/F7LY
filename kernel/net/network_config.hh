#pragma once

namespace net
{
// 网络层的静态默认配置，与网卡寄存器、DMA 和收发实现无关。
// 后续若接入 DHCP，只需替换配置来源，不必修改任何硬件驱动。
struct NetworkConfig
{
    const char *ipv4_address;
    const char *ipv4_netmask;
    const char *ipv4_gateway;
    const char *ipv4_dns;
    const char *ipv4_broadcast;
};

// 当前构建目标的平台配置提供此函数；返回对象在内核整个生命周期内有效。
const NetworkConfig &default_network_config();
} // namespace net

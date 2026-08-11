#pragma once

#include "net/network_config.hh"

namespace platform::qemu
{
// QEMU user-mode networking 的固定访客网段。RISC-V 与 LoongArch 使用同一
// 网络模型，因此只保留一份权威常量。
inline constexpr net::NetworkConfig k_default_network_config{
    .ipv4_address = "10.0.2.15",
    .ipv4_netmask = "255.255.255.0",
    .ipv4_gateway = "10.0.2.2",
    .ipv4_dns = "10.0.2.3",
    .ipv4_broadcast = "10.0.2.255",
};
} // namespace platform::qemu

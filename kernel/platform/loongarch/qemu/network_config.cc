#include "net/network_config.hh"

#include "platform/qemu/network_defaults.hh"

namespace net
{
const NetworkConfig &default_network_config()
{
    return platform::qemu::k_default_network_config;
}
} // namespace net

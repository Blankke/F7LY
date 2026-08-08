#include "platform_network.hh"

#include "f7ly_network.hh"

namespace net
{
bool init_platform_network()
{
    return init_network_stack();
}
} // namespace net

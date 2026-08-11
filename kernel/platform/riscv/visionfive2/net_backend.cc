/* SPDX-License-Identifier: GPL-3.0-only */
#include "net/drivers/net_backend.hh"

#include "net/drivers/riscv/jh7110_gmac.hh"

namespace net::backend
{
bool initialize()
{
    return riscv::jh7110::gmac::initialize();
}

int send(const void *data, uint32 length)
{
    return riscv::jh7110::gmac::send(data, length);
}

int receive(void *data, uint32 *length)
{
    return riscv::jh7110::gmac::receive(data, length);
}

void poll()
{
    riscv::jh7110::gmac::poll();
}

void get_mac(uint8 mac[platform_device::k_mac_address_length])
{
    riscv::jh7110::gmac::get_mac(mac);
}

const char *name()
{
    return "gmac1";
}

void debug_status()
{
    riscv::jh7110::gmac::debug_status();
}
} // namespace net::backend

/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "types.hh"

namespace riscv::jh7110::gmac
{
bool initialize();
int send(const void *data, uint32 length);
int receive(void *data, uint32 *length);
void poll();
void get_mac(uint8 mac[6]);
void debug_status();
} // namespace riscv::jh7110::gmac

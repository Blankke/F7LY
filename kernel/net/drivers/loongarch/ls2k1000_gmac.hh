#pragma once

#include "types.hh"

namespace loongarch::ls2k1000::gmac
{
bool initialize();
int send(const void *data, uint32 length);
int receive(void *data, uint32 *length);
void poll();
void get_mac(uint8 mac[6]);
void debug_status();
} // namespace loongarch::ls2k1000::gmac

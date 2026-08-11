#pragma once

#ifdef LOONGARCH

#include "types.hh"

void extioi_init(void);
bool extioi_enable(uint32 source);
uint64 extioi_claim(void);
void extioi_complete(uint64);

#endif

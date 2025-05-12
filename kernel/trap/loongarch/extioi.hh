#pragma once

#ifdef LOONGARCH

#include "types.hh"

void extioi_init(void);
uint64 extioi_claim(void);
void extioi_complete(uint64);

#endif
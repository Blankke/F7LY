#pragma once

#ifdef LOONGARCH

#include "types.hh"

void apic_init(void);

void apic_complete(uint64 irq);

#endif
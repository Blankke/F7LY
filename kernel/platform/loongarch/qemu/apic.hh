#pragma once

#include "types.hh"

void apic_init(void);

bool apic_enable(uint32 source);

void apic_complete(uint64 irq);

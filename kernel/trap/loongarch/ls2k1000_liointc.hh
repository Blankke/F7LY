#pragma once

#include "types.hh"

namespace loongarch::liointc
{
    void init(uint32 enabled_inputs);
    uint32 claim();
    void complete(uint32 inputs);
}

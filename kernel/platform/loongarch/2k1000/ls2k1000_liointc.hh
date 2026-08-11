#pragma once

#include "types.hh"

namespace loongarch::liointc
{
    void init(uint32 enabled_inputs);
    bool enable(uint32 input);
    uint32 claim();
    void complete(uint32 inputs);
}

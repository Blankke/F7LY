#pragma once

#include "types.hh"

namespace perfdiag::detail
{
    inline uint64 hash_pc(uint64 pc)
    {
        pc ^= pc >> 33;
        pc *= 0xff51afd7ed558ccdULL;
        pc ^= pc >> 33;
        return pc;
    }

    inline uint64 hash_chain(const uint64 *pcs, uint8 depth)
    {
        uint64 hash = 1469598103934665603ULL;
        for (uint8 i = 0; i < depth; ++i)
        {
            hash ^= pcs[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    inline bool stack_word_valid(uint64 address, uint64 bottom, uint64 top)
    {
        return top >= sizeof(uint64) && (address & 7ULL) == 0 &&
               address >= bottom && address <= top - sizeof(uint64);
    }

    inline bool next_frame_valid(uint64 current, uint64 next, uint64 top)
    {
        return next > current && next <= top && (next & 15ULL) == 0;
    }

    inline uint64 aggregate_max(uint64 current, uint64 candidate)
    {
        return candidate > current ? candidate : current;
    }

    inline uint64 epoch_value(uint64 slot_epoch, uint64 current_epoch, uint64 value)
    {
        return slot_epoch == current_epoch ? value : 0;
    }
}

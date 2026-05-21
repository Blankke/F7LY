#pragma once
#include "types.hh"

struct DtbMemoryRegion
{
    uint64 base;
    uint64 size;
};

class DtbManager {
public:
    static constexpr int k_max_memory_regions = 8;

    static void init(uint64 dtb_addr);
    static bool get_initrd(uint64& start, uint64& end);
    static void find_dtb_and_initrd(uint64 passed_dtb_addr, uint64 kernel_end_phys);
    static int get_memory_regions(DtbMemoryRegion *regions, int max_regions);
private:
    static uint64 _dtb_addr;
};

extern uint64 k_dtb_addr;
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;

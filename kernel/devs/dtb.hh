#pragma once
#include "types.hh"

class DtbManager {
public:
    static void init(uint64 dtb_addr);
    static bool get_initrd(uint64& start, uint64& end);
    static void find_dtb_and_initrd(uint64 passed_dtb_addr, uint64 kernel_end_phys);
private:
    static uint64 _dtb_addr;
};

extern uint64 k_dtb_addr;
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;

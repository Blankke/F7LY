#pragma once
#include "types.hh"

class DtbManager {
public:
    static void init(uint64 dtb_addr);
    static bool get_initrd(uint64& start, uint64& end);
private:
    static uint64 _dtb_addr;
};

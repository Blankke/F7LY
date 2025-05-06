#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "mem/kalloc.hh"
#include "vm.hh"
#include "slab.hh"
#include "pagetable.hh"

__attribute__ ((aligned (16))) char stack0[NCPU][4096*2];

extern "C" void main() {
    k_printer.init();
    printfRed("FUCK\n");
    printfMagenta("YOU!\n");
	kvminithart(); // turn on paging
    printfRed("FUCK\n");
    
    mem::KAlloc::init();// 伙伴分配器初始化
    printfRed("YOU!\n");
}

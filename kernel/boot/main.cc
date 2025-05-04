#include "uart.hh"
#include "printer.hh"
#include "../hal/riscv/sbi.hh"
#include "param.h"
#include "mem/kalloc.hh"
#include "vm.hh"



extern "C" void main() {
    k_printer.init();
    k_printer.printf("Hello, World!\n");
    
    
    mem::KAlloc::init();// 伙伴分配器初始化
    kvminit(); // create kernel page table
	kvminithart(); // turn on paging

}

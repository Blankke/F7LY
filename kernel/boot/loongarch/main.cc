#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "mem/kalloc.hh"
#include "vm.hh"



extern "C" void main() {
    k_printer.init();
    printfRed("Hello, World!\n");
    Info("Hello, World!\n");
    printfCYAN("Hello, World!\n");
    printfYELLOW("Hello, World!\n");
    
    mem::KAlloc::init();// 伙伴分配器初始化
    kvminit(); // create kernel page table
	kvminithart(); // turn on paging

}

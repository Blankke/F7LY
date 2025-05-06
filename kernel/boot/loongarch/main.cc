#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "kalloc.hh"
#include "vm.hh"



extern "C" void main() {
    k_printer.init();
    printfRed("Hello, World!\n");
    Info("Hello, World!\n");
    printfCyan("Hello, World!\n");
    printfYellow("Hello, World!\n");
    
    mem::KAlloc::init();// 伙伴分配器初始化
    kvminit(); // create kernel page table
	kvminithart(); // turn on paging

}

#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "slab.hh"
#include "pagetable.hh"
#include "fuckyou.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"

__attribute__ ((aligned (16))) char stack0[NCPU][4096*2];


void main() {
    k_printer.init();
    printfWhite("\n\n");  // 留出顶部空白
    print_fuckyou_horizontal();
    printfWhite("\n\n");  // 底部空白
    mem::k_pmm.init();
    printfYellow("[pmm] PhysicalMemoryManager init success\n");
    mem::k_vmm.init("virtual_memory_manager");
    printfYellow("[vmm] VirtualMemoryManager init success\n");
    mem::k_hmm.init("heap_memory_manager");
    printfYellow("[hmm] HeapMemoryManager init success\n");
    
}

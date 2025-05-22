#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "slab.hh"
#include "pagetable.hh"
#include "fuckyou.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"
#include "trap.hh"
#include "trap/plic.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"

// 注意华科的main函数可能有问题, 注意多核初始化
void main() {

    k_printer.init(); // 这里也初始化了console和uart

    printfWhite("\n\n");  // 留出顶部空白
    print_f7ly();
    print_fuckyou();
    printfWhite("\n\n");  // 底部空白
    trap_mgr.init();// trap初始化
    trap_mgr.inithart();// 初始化每个核上的csr
    printfRed("trap YOU!\n");

    plic_mgr.init();// plic初始化
    plic_mgr.inithart();// 初始化每个核上的csr
    printfRed("plic YOU!\n");
    
    mem::k_pmm.init();
    printfYellow("[pmm] PhysicalMemoryManager init success\n");
    mem::k_vmm.init("virtual_memory_manager");
    printfYellow("[vmm] VirtualMemoryManager init success\n");
    mem::k_hmm.init("heap_memory_manager",HEAP_START);
    printfYellow("[hmm] HeapMemoryManager init success,heap_start: %p\n",HEAP_START);

    proc::k_pm.init("next pid", "wait lock");
    printfMagenta("k_pm you\n");

    mem::PageTable* pt = new mem::PageTable();
    pt->set_base(0x80000000);
    printfYellow("pt->addr: %p\n",pt);
    delete pt;
    string str("gkq全世界最帅");
    k_printer<<str<<Printer::endl;
}

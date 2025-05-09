#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "slab.hh"
#include "pagetable.hh"
#include "fuckyou.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"
#include "trap/trap.hh"
#include "trap/plic.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
__attribute__ ((aligned (16))) char stack0[NCPU][4096*2];

// 注意华科的main函数可能有问题
void main() {
    k_printer.init();
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
    mem::k_hmm.init("heap_memory_manager");
    printfYellow("[hmm] HeapMemoryManager init success\n");

    // proc::k_pm.init("next pid", "wait lock");
    printfMagenta("k_pm you\n");

    printfRed("FUCK\n");
    printfMagenta("YOU!\n");



}

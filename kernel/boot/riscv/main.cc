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
#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
#include "fs/vfs/buffer.hh"
#include "fs/vfs/buffer_manager.hh"
#include "hal/riscv/sbi.hh"
#include "fs/vfs/path.hh"
#include "fs/vfs/dentrycache.hh"
#include "fs/ramfs/ramfs.hh"
#include "tm/timer_manager.hh"
#include "proc/scheduler.hh"


// 注意华科的main函数可能有问题, 注意多核初始化
void main() {
    // riscv::r_mstatus();

    uint64 x;
    asm volatile("csrr %0, sstatus" : "=r"(x));

    k_printer.init(); // 这里也初始化了console和uart

    printfWhite("\n\n");  // 留出顶部空白
    print_f7ly();
    print_fuckyou();
    printfWhite("\n\n");  // 底部空白
    trap_mgr.init();// trap初始化
    trap_mgr.inithart();// 初始化每个核上的csr

    plic_mgr.init();// plic初始化
    plic_mgr.inithart();// 初始化每个核上的csr

    mem::k_pmm.init();
    mem::k_vmm.init("virtual_memory_manager");
    mem::k_hmm.init("heap_memory_manager",HEAP_START);


    proc::k_pm.init("next pid", "wait lock");
    tmm::k_tm.init("timer manager");
    fs::k_bufm.init("buffer manager");
    new (&fs::dentrycache::k_dentryCache) fs::dentrycache::dentryCache;
    fs::dentrycache::k_dentryCache.init();
    new (&fs::mnt_table) eastl::unordered_map<eastl::string, fs::FileSystem *>;
    fs::mnt_table.clear(); // clean mnt_Table
    new (&fs::ramfs::k_ramfs) fs::ramfs::RamFS;
    fs::ramfs::k_ramfs.initfd();
    fs::mnt_table["/"] = &fs::ramfs::k_ramfs;
    fs::Path mnt("/mnt");
    fs::Path dev("/dev/hdb");
    mnt.mount(dev, "ext4", 0, 0);

    // proc::k_pm.user_init(); // 初始化用户进程
    // printfMagenta("user init\n");

    // proc::k_scheduler.init("scheduler");
    // proc::k_scheduler.start_schedule(); // 启动调度器

    printfMagenta("\n"
                  "╦ ╦╔═╗╦  ╔═╗╔═╗╔╦╗╔═╗\n"
                  "║║║║╣ ║  ║  ║ ║║║║║╣\n"
                  "╚╩╝╚═╝╩═╝╚═╝╚═╝╩ ╩╚═╝\n"
                  "\n"
                  "=== SYSTEM BOOT COMPLETE ===\n"
                  "Kernel space successfully initialized\n"); // ANSI Shadow 字体风格

    sbi_shutdown();
}

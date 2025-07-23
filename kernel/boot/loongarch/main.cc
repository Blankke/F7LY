#include "devs/uart.hh"
#include "printer.hh"
#include "param.h"
#include "apic.hh"
#include "mem/memlayout.hh"
#include "trap.hh"
#include "extioi.hh"
#include "proc/proc_manager.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/virtual_memory_manager.hh"
#include "mem/heap_memory_manager.hh"
#include "device_manager.hh"
#include "disk_driver.hh"
#include "devs/console1.hh"
#include "fs/vfs/buffer.hh"
#include "fs/vfs/buffer_manager.hh"
#include "tm/timer_manager.hh"
#include "syscall_handler.hh"
#include "scheduler.hh"
#include "fs/dev/acpi_controller.hh"
#include "trap/interrupt_stats.hh"
#ifdef LOONGARCH

extern "C" void main()
{
    k_printer.init();
    printfYellow("Hello, World!\n");
    dev::acpi::k_acpi_controller.init("acpi", 0x100E001C | DMWIN1_MASK);
    apic_init();
    extioi_init();

    trap_mgr.init();
    trap_mgr.inithart();
    
    // 初始化中断统计管理器
    intr_stats::k_intr_stats.init();
    
    proc::k_pm.init("next pid", "next tid", "wait lock");
    mem::k_pmm.init();

    mem::k_vmm.init("virtual_memory_manager");
    shm::k_smm.init(SHM_START, SHM_SIZE); // 初始化共享内存管理器
    mem::k_hmm.init("heap_memory_manager", HEAP_START);

    if (dev::k_devm.register_stdin(static_cast<dev::VirtualDevice *>(&dev::k_stdin)) < 0)
        while (1)
            ;
    if (dev::k_devm.register_stdout(static_cast<dev::VirtualDevice *>(&dev::k_stdout)) < 0)
        while (1)
            ;
    if (dev::k_devm.register_stderr(static_cast<dev::VirtualDevice *>(&dev::k_stderr)) < 0)
        while (1)
            ;
    ///@todo: 这里的 disk_driver 有问题
    new (&loongarch::qemu::disk_driver) loongarch::qemu::DiskDriver("Disk");
    tmm::k_tm.init("timer manager");
    fs::k_bufm.init("buffer manager");
    syscall::k_syscall_handler.init(); // 初始化系统调用处理器
    proc::k_pm.user_init();            // 初始化用户进程
    printfMagenta("user init\n");
    proc::k_scheduler.init("scheduler");
    proc::k_scheduler.start_schedule();       // 启动调度器
    dev::acpi::k_acpi_controller.power_off(); // 关机
}

#endif

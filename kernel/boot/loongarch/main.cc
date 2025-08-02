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
#include "loongarch/disk_driver.hh"
#include "tm/timer_manager.hh"
#include "syscall_handler.hh"
#include "scheduler.hh"
#include "slab.hh"
#include "trap/interrupt_stats.hh"
#include "shm/shm_manager.hh"
#include "fs/drivers/riscv/virtio2.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/virtual_fs.hh"
#include "loop_device.hh"
#include "fs/vfs/fifo_manager.hh"
#ifdef LOONGARCH

extern "C" void main()
{
    k_printer.init();
    printfYellow("Hello, World!\n");

    apic_init();
    extioi_init();

    trap_mgr.init();
    trap_mgr.inithart();

    // 初始化中断统计管理器
    intr_stats::k_intr_stats.init();

    proc::k_pm.init("next pid", "next tid", "wait lock");
    mem::k_pmm.init();

    mem::k_vmm.init("virtual_memory_manager");
    mem::k_hmm.init("heap_memory_manager", HEAP_START);
    shm::k_smm.init(SHM_START, SHM_SIZE); // 初始化共享内存管理器

    mem::SlabAllocator::init(); // 初始化 SlabAllocator
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
    // new (&loongarch::qemu::disk_driver) loongarch::qemu::DiskDriver("Disk");
    tmm::k_tm.init("timer manager");

    syscall::k_syscall_handler.init(); // 初始化系统调用处理器
    proc::k_pm.user_init();            // 初始化用户进程

    /*********************8888 */

    // virtio_disk_init2(); // 初始化 rootfs的块设备
    virtio_probe();             //曹老师漏了这个
    virtio_disk_init();        // emulated hard disk ps:如果使用SDCard需要修改
    init_fs_table();           // fs_table init
    binit();                   // buffer cache
    fileinit();                // file table
    inodeinit();               // inode table
    fs::k_file_table.init();   // 初始化文件池
    vfs_ext4_init();           // 初始化lwext4
    fs::k_vfs.dir_init();      // 初始化虚拟文件系统目录
    fs::k_fifo_manager.init(); // 初始化 FIFO 管理器
    // 初始化 loop 设备控制器
    dev::LoopControlDevice::init_loop_control();
    /************************* */
    printfMagenta("user init\n");
    proc::k_scheduler.init("scheduler");
    proc::k_scheduler.start_schedule(); // 启动调度器
}

#endif

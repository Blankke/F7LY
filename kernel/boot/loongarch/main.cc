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
#include "devs/dtb.hh"
#include "loongarch/disk_driver.hh"
#include "tm/timer_manager.hh"
#include "syscall_handler.hh"
#include "scheduler.hh"
#include "slab.hh"
#include "trap/interrupt_stats.hh"
#include "shm/shm_manager.hh"
#include "fs/drivers/virtio_blk.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/virtual_fs.hh"
#include "loop_device.hh"
#include "fs/vfs/fifo_manager.hh"
#include "hal/smp.hh"
#ifdef LOONGARCH

extern char end[];

extern "C" void main(uint64 hartid, uint64 dtb_addr)
{
    hal::smp::enter(hartid, dtb_addr);

    k_printer.init();
    printfYellow("Hello, World!\n");

    // Initialize DTB and scan Initrd if necessary
    uint64 kernel_end_phys = ((uint64)end) & 0x0FFFFFFFFFFFFFFFUL;
    DtbManager::find_dtb_and_initrd(dtb_addr, kernel_end_phys);
    hal::smp::configure_topology();
    
    printfMagenta("[main] Using hartid=%lu, k_dtb_addr=0x%lx\n", hartid, k_dtb_addr);

    apic_init();
    extioi_init();

    trap_mgr.init();

    // 初始化中断统计管理器
    intr_stats::k_intr_stats.init();

    proc::k_pm.init("next pid", "next tid", "wait lock");
    mem::k_pmm.init();

    mem::k_vmm.init("virtual_memory_manager");
    mem::k_hmm.init("heap_memory_manager", mem::k_pmm.get_heap_area_start(), mem::k_pmm.get_heap_allocator_size());
    shm::k_smm.init(mem::k_pmm.get_shm_start(), mem::k_pmm.get_shm_size()); // 初始化共享内存管理器

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

    // virtio_probe()/virtio_disk_init() 会依赖块设备完成中断；这里在全局
    // 进程、内存、时间对象都初始化完成后才开启主核本地 trap。
    trap_mgr.inithart();

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
    // APIC/ExtIOI 是主核已完成的全局配置；次核在 bootstrap gate 后只开启
    // 自己的 timer/trap CSR，并在完整初始化后对调度器宣布 online。全部
    // possible CPU online 前不允许任意核开始调度用户任务。
    // LoongArch QEMU 不会自动放行次核，需先用 IOCSR mailbox/IPI 发送入口。
    // 次核在 gate 前只会自旋，因此这里不会和仍在执行的主核初始化并发。
    hal::smp::start_secondaries(dtb_addr);
    proc::k_scheduler.start_schedule(); // 启动调度器
}

#endif

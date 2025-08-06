#include "uart.hh"
#include "printer.hh"
#include "param.h"
#include "slab.hh"
#include "mem/riscv/pagetable.hh"
#include "fuckyou.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"
#include "trap.hh"
#include "riscv/plic.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
// #include "fs/vfs/buffer.hh"
// #include "fs/vfs/buffer_manager.hh"
#include "hal/riscv/sbi.hh"
// #include "fs/vfs/path.hh"
// #include "fs/vfs/dentrycache.hh"
// #include "fs/ramfs/ramfs.hh"
#include "tm/timer_manager.hh"
#include "proc/scheduler.hh"
#include "syscall_handler.hh"
#include "devs/device_manager.hh"
#include "devs/loop_device.hh"
#include "fs/vfs/file/device_file.hh"
#include "devs/console1.hh"
#include "fs/vfs/inode.hh"
#include "mem/userspace_stream.hh"
#include "trap/interrupt_stats.hh"
// #include "fs/dev/acpi_controller.hh"
#include "fs/drivers/riscv/virtio2.hh"
#include "fs/vfs/fs.hh"
#include "fs/buf.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/virtual_fs.hh"
#include "shm/shm_manager.hh"
#include "fs/vfs/fifo_manager.hh"
#include "net/drivers/virtio_net.hh"
#include "net/f7ly_network.hh"
// 注意华科的main函数可能有问题, 注意多核初始化
void main()
{
    // riscv::r_mstatus();

    k_printer.init(); // 这里也初始化了console和uart
    printfWhite("\n\n"); // 留出顶部空白
    print_f7ly();
    print_fuckyou();
    printfWhite("\n\n"); // 底部空白
    trap_mgr.init();     // trap初始化
    trap_mgr.inithart(); // 初始化每个核上的csr

    // 初始化中断统计管理器
    // intr_stats::k_intr_stats.init();

    plic_mgr.init();     // plic初始化
    plic_mgr.inithart(); // 初始化每个核上的csr

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

    // hardware_secondary_init
    //  2. Disk 初始化 (debug)

    tmm::k_tm.init("timer manager");
    // fs::k_bufm.init("buffer manager");

    syscall::k_syscall_handler.init(); // 初始化系统调用处理器

    proc::k_pm.user_init(); // 初始化用户进程

    /*********************8888 */

    // virtio_disk_init2(); // 初始化 rootfs的块设备
    virtio_disk_init();  // emulated hard disk ps:如果使用SDCard需要修改
    init_fs_table();     // fs_table init
    binit();             // buffer cache
    fileinit();          // file table
    inodeinit();         // inode table
    fs::k_file_table.init(); // 初始化文件池
    vfs_ext4_init();      // 初始化lwext4
    fs::k_vfs.dir_init(); // 初始化虚拟文件系统目录
    fs::k_fifo_manager.init(); // 初始化 FIFO 管理器
    // 初始化 loop 设备控制器
    dev::LoopControlDevice::init_loop_control();
        /************************* */

        printfMagenta("user init\n");

    printfMagenta("\n"
                  "╦ ╦╔═╗╦  ╔═╗╔═╗╔╦╗╔═╗\n"
                  "║║║║╣ ║  ║  ║ ║║║║║╣\n"
                  "╚╩╝╚═╝╩═╝╚═╝╚═╝╩ ╩╚═╝\n"
                  "\n"
                  "=== SYSTEM BOOT COMPLETE ===\n"
                  "Kernel space successfully initialized\n"); // ANSI Shadow 字体风格

    proc::k_scheduler.init("scheduler");
    proc::k_scheduler.start_schedule(); // 启动调度器
    sbi_shutdown();
}

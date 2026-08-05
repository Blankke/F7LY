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
#include "fs/drivers/virtio_blk.hh"
#include "fs/vfs/fs.hh"
#include "fs/buf.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/virtual_fs.hh"
#include "shm/shm_manager.hh"
#include "fs/vfs/fifo_manager.hh"
#include "net/drivers/virtio_net.hh"
#include "net/f7ly_network.hh"
#include "devs/dtb.hh"
#include "hal/smp.hh"

extern uint64 k_dtb_addr;

extern "C" void main(uint64 hartid, uint64 dtb_addr)
{
    hal::smp::enter(hartid, dtb_addr);

    // 只有主核做一次性初始化。次核会在 bootstrap_ready 的 release/acquire 栅栏后
    // 执行本地 trap/PLIC/调度器初始化，避免重复清空全局进程、内存和文件系统状态。
    k_dtb_addr = dtb_addr;
    DtbManager::init(dtb_addr);
    hal::smp::configure_topology();
    // riscv::r_mstatus();

    k_printer.init(); // 这里也初始化了console和uart
    printfWhite("\n\n"); // 留出顶部空白
    print_f7ly();
    print_fuckyou();
    printfWhite("\n\n"); // 底部空白
    trap_mgr.init(); // 全局 trap 状态只初始化一次

    // 初始化中断统计管理器
    intr_stats::k_intr_stats.init();

    plic_mgr.init(); // PLIC IRQ 优先级是全局寄存器，只能由主核配置一次

    proc::k_pm.init("next pid", "next tid", "wait lock");
    // user_init() 会发布第一个 RUNNABLE PCB，并由调度器完成 home CPU 与
    // 压力记账；调度器必须先初始化，不能在发布后再把计数清零。
    proc::k_scheduler.init("scheduler");

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

    // hardware_secondary_init
    //  2. Disk 初始化 (debug)

    tmm::k_tm.init("timer manager");
    // fs::k_bufm.init("buffer manager");

    syscall::k_syscall_handler.init(); // 初始化系统调用处理器

    proc::k_pm.user_init(); // 初始化用户进程

    /*********************8888 */

    // virtio 块设备初始化会提交异步请求；必须先打开主核本地 trap/PLIC，
    // 否则 used ring 无法及时回收，后续文件系统初始化会读到失效请求指针。
    // 此时进程、内存与时间子系统均已就绪，早期中断不会碰到半初始化对象。
    trap_mgr.inithart();
    plic_mgr.inithart();

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

    // 所有全局对象均已完成初始化后，才放行次核；主核本地中断已在
    // virtio 初始化前启用，次核只做自己的本地 trap/PLIC 配置。之后等待
    // 全部 possible CPU 报告 online，再让任何 CPU 开始调度用户任务。
    hal::smp::start_secondaries(dtb_addr);

    proc::k_scheduler.start_schedule(); // 启动调度器
    sbi_shutdown();
}

#include "kernel_init.hh"

#include "devs/console1.hh"
#include "devs/device_manager.hh"
#include "devs/loop_device.hh"
#include "fs/buf.hh"
#include "fs/drivers/platform_block.hh"
#include "fs/vfs/fifo_manager.hh"
#include "fs/vfs/file.hh"
#include "fs/vfs/file/file.hh"
#include "fs/vfs/fs.hh"
#include "fs/vfs/inode.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "fs/vfs/virtual_fs.hh"
#include "hal/irq.hh"
#include "hal/smp.hh"
#include "mem/heap_memory_manager.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/slab.hh"
#include "mem/virtual_memory_manager.hh"
#include "platform/power.hh"
#include "platform/profile.hh"
#include "printer.hh"
#include "proc/proc_manager.hh"
#include "proc/file_page_cache.hh"
#include "proc/scheduler.hh"
#include "shm/shm_manager.hh"
#include "syscall_handler.hh"
#include "tm/platform_rtc.hh"
#include "tm/timer_manager.hh"
#include "trap.hh"
#include "trap/interrupt_stats.hh"

namespace boot
{
namespace
{
void register_standard_streams()
{
    if (dev::k_devm.register_stdin(
            static_cast<dev::VirtualDevice *>(&dev::k_stdin)) < 0 ||
        dev::k_devm.register_stdout(
            static_cast<dev::VirtualDevice *>(&dev::k_stdout)) < 0 ||
        dev::k_devm.register_stderr(
            static_cast<dev::VirtualDevice *>(&dev::k_stderr)) < 0)
    {
        panic("[boot] failed to register standard streams");
    }
}

void initialize_memory_and_core_services()
{
    proc::k_pm.init("next pid", "next tid", "wait lock");
    // user_init() 会发布第一个 RUNNABLE PCB，调度器必须先建立统计与队列。
    proc::k_scheduler.init("scheduler");

    mem::k_pmm.init();
    boardPrintfInfo("[boot] stage=memory pmm ready\n");
    mem::k_vmm.init("virtual_memory_manager");
    boardPrintfInfo("[boot] stage=memory vmm ready\n");
    mem::k_hmm.init("heap_memory_manager",
                    mem::k_pmm.get_heap_area_start(),
                    mem::k_pmm.get_heap_allocator_size());
    boardPrintfInfo("[boot] stage=memory heap ready\n");
    // 文件页缓存依赖已经可用的 PMM/VMM/HMM，但必须在首个用户进程发布前初始化。
    proc::file_page_cache::init();
    shm::k_smm.init(mem::k_pmm.get_shm_start(), mem::k_pmm.get_shm_size());
    mem::SlabAllocator::init();
    register_standard_streams();

    tmm::k_tm.init("timer manager");
    (void)tmm::initialize_platform_realtime();
    syscall::k_syscall_handler.init();
    proc::k_pm.user_init();
}

void initialize_storage_and_vfs()
{
    if (!platform_block_init())
    {
        panic("[boot] platform block initialization failed");
    }

    init_fs_table();
    binit();
    fileinit();
    inodeinit();
    fs::k_file_table.init();
    vfs_ext4_init();
    fs::k_vfs.dir_init();
    fs::k_fifo_manager.init();
    dev::LoopControlDevice::init_loop_control();
}
} // namespace

[[noreturn]] void initialize_kernel(const platform::BootInfo &boot_info)
{
    const platform::Profile &profile = platform::current_profile();
    boardPrintfInfo("[boot] profile=%s cpu=%lu dtb=0x%lx\n",
                    profile.name, boot_info.boot_cpu_hwid,
                    boot_info.device_tree_paddr);

    // handler 先由 Console 注册，控制器随后只开放已经有 owner 的 source。
    boardPrintfInfo("[boot] stage=interrupts begin\n");
    trap_mgr.init();
    intr_stats::k_intr_stats.init();
    hal::irq::initialize_global();
    boardPrintfInfo("[boot] stage=interrupts ready sources=0x%lx\n",
                    hal::irq::registered_sources());

    boardPrintfInfo("[boot] stage=kernel-services begin\n");
    initialize_memory_and_core_services();
    boardPrintfInfo("[boot] stage=kernel-services ready\n");

    // 块设备可能在初始化时等待 IRQ，故当前 CPU 的 trap vector 和控制器 context
    // 必须先完整就绪；未注册的设备 source 仍保持屏蔽。
    hal::irq::initialize_current_cpu();
    trap_mgr.inithart();

    boardPrintfInfo("[boot] stage=storage begin\n");
    initialize_storage_and_vfs();
    boardPrintfInfo("[boot] stage=storage ready\n");

    // 只有所有全局对象、根盘和 VFS 都稳定后才发布 bootstrap gate。
    boardPrintfInfo("[boot] stage=smp begin\n");
    hal::smp::start_secondaries(boot_info.device_tree_paddr);
    boardPrintfInfo("[boot] stage=scheduler start\n");
    proc::k_scheduler.start_schedule();

    platform::power::shutdown();
}
} // namespace boot

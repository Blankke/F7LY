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
#include "hal/cpu.hh"
#include "hal/loongarch/smp.hh"
#ifdef LOONGARCH

extern char end[];
extern "C" char _entry[];

namespace
{
    [[noreturn]] void park_unmanaged_cpu()
    {
        // LoongArch 没有依赖外部固件的 hart-stop 路径；这里保持本地空转，
        // 并且绝不访问未初始化的 Cpu 槽位或全局锁。
        for (;;)
        {
            asm volatile("nop");
        }
    }

    void configure_cpu_topology()
    {
        uint64 hartids[NCPU] = {};
        int hart_count = DtbManager::get_cpu_hartids(hartids, NCPU);
        if (hart_count == 0)
        {
            hartids[0] = Cpu::bootstrap_cpu_id();
            hart_count = 1;
        }

        Cpu::configure_topology(hartids, hart_count);
        printfGreen("[smp] LoongArch possible cpu mask=0x%lx count=%d\n",
                    Cpu::possible_cpu_mask(), Cpu::possible_cpu_count());
    }

    [[noreturn]] void secondary_main(uint64 hartid)
    {
        // QEMU LoongArch virt 的次核先停在 flash 的 slave boot ROM；主核
        // 通过 IOCSR mailbox/IPI 跳转到本入口后，次核仍须等待全局对象完成
        // 初始化，随后才可启用本地 timer interrupt。
        Cpu::wait_for_bootstrap_ready();
        if (!Cpu::is_possible_cpu(hartid))
        {
            park_unmanaged_cpu();
        }

        // LoongArch 的 PGDL/PGDH 与 TLB 也是每核私有状态；次核必须在开中断
        // 前接入主核完成的内核地址空间，否则中断路径无法访问高地址内核栈。
        mem::k_vmm.activate_kernel_pagetable();

        Cpu::initialize_current();
        trap_mgr.inithart();
        Cpu::mark_current_online();
        printfGreen("[smp] LoongArch cpu%lu online\n", hartid);

        // 不允许第一个上线的次核立刻调度 initcode；主核必须先确认全部
        // possible CPU 都已完成本地 trap/CPU 初始化，再统一打开 scheduler。
        Cpu::wait_for_scheduler_ready();
        proc::k_scheduler.start_schedule();
        park_unmanaged_cpu();
    }

    void start_discovered_secondary_cpus(uint64 dtb_addr)
    {
        const uint64 primary_cpu = Cpu::current_cpu_id();
        const uint64 possible_mask = Cpu::possible_cpu_mask();
        const uint64 entry = reinterpret_cast<uint64>(_entry);

        for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
        {
            if (cpu_id == primary_cpu || (possible_mask & (1ULL << cpu_id)) == 0)
            {
                continue;
            }

            // DTB 参数当前只由主核解析；仍写入 mailbox1，保持入口 ABI 完整，
            // 也让后续把次核入口拆分为独立 trampoline 时无需变更启动协议。
            loongarch::smp::start_secondary_cpu(cpu_id, entry, dtb_addr);
            printfGreen("[smp] LoongArch requested cpu%lu entry=0x%lx\n", cpu_id, entry);
        }
    }
}

extern "C" void main(uint64 hartid, uint64 dtb_addr)
{
    if (!Cpu::is_valid_cpu_id(hartid))
    {
        park_unmanaged_cpu();
    }
    if (!Cpu::try_claim_bootstrap())
    {
        secondary_main(hartid);
    }

    Cpu::bootstrap_begin();
    k_printer.init();
    printfYellow("Hello, World!\n");

    // Initialize DTB and scan Initrd if necessary
    uint64 kernel_end_phys = ((uint64)end) & 0x0FFFFFFFFFFFFFFFUL;
    DtbManager::find_dtb_and_initrd(dtb_addr, kernel_end_phys);
    configure_cpu_topology();
    
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
    start_discovered_secondary_cpus(dtb_addr);
    Cpu::publish_bootstrap_ready();
    Cpu::wait_for_all_possible_cpus_online();
    Cpu::publish_scheduler_ready();
    proc::k_scheduler.start_schedule(); // 启动调度器
}

#endif

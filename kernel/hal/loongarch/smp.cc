#include "smp.hh"

#include "devs/dtb.hh"
#include "hal/cpu.hh"
#include "hal/irq.hh"
#include "hal/smp.hh"
#include "hal/tlb_shootdown.hh"
#include "mem/memlayout.hh"
#include "mem/virtual_memory_manager.hh"
#include "hal/arch.hh"
#include "printer.hh"
#include "proc/scheduler.hh"
#include "tm/time.hh"
#include "trap.hh"

extern "C" char _secondary_entry[];

namespace loongarch::smp
{
namespace
{
    // LoongArch 固件停驻代码等待 MAIL_BUF0 入口并由 IPI 唤醒。2K1000 与
    // QEMU 都使用该 IOCSR 机制；MAIL_BUF1 保留给次核启动参数。
    constexpr uint64 k_iocsr_ipi_send = 0x1040;
    constexpr uint64 k_iocsr_mailbox_send = 0x1048;
    constexpr uint64 k_iocsr_send_blocking = 1ULL << 31;
    constexpr uint64 k_iocsr_target_cpu_shift = 16;
    constexpr uint64 k_iocsr_mailbox_shift = 2;
    constexpr uint64 k_iocsr_high_word_mask = 0xffffffff00000000ULL;
    constexpr uint32 k_runtime_ipi_vector = 1;

    // mailbox 传输寄存器一次只写目标 mailbox 的 32 位。BOX 的
    // 编码是 mailbox*2（低 32 位）或 mailbox*2+1（高 32 位）。
    inline void write_mailbox(uint64 cpu_id, uint64 mailbox, uint64 value)
    {
        const uint64 target = k_iocsr_send_blocking |
                              (cpu_id << k_iocsr_target_cpu_shift);
        const uint64 high_word = target |
                                 (((mailbox * 2 + 1) << k_iocsr_mailbox_shift)) |
                                 (value & k_iocsr_high_word_mask);
        const uint64 low_word = target |
                                ((mailbox * 2) << k_iocsr_mailbox_shift) |
                                (value << 32);

        asm volatile("iocsrwr.d %0, %1" : : "r"(high_word), "r"(k_iocsr_mailbox_send) : "memory");
        asm volatile("iocsrwr.d %0, %1" : : "r"(low_word), "r"(k_iocsr_mailbox_send) : "memory");
    }

    inline void send_ipi(uint64 cpu_id, uint32 vector)
    {
        // IOCSR_IPI_SEND 的控制位宽为 32 位，使用 iocsrwr.w 与设备寄存器
        // 宽度保持一致。vector 0 用于 slave boot，vector 1 用于 runtime。
        const uint32 value = static_cast<uint32>(k_iocsr_send_blocking |
                                                  (cpu_id << k_iocsr_target_cpu_shift) |
                                                  vector);
        asm volatile("iocsrwr.w %0, %1" : : "r"(value), "r"(k_iocsr_ipi_send) : "memory");
    }
}

void start_secondary_cpu(uint64 cpu_id, uint64 entry, uint64 argument)
{
    // 固件停驻代码在 DA 模式下跳转到 MAIL_BUF0，入口必须使用缓存 DMW
    // 别名。次核进入独立 _secondary_entry，不再复用并清理主入口状态。
    const uint64 cached_entry = loongarch::board::cached_address(entry);

    // 先把所有由次核观察的内存状态发布出去，再写 mailbox 并发 IPI；这与
    // LoongArch 的 dbar 0 启动序列保持一致。
    asm volatile("dbar 0" ::: "memory");
    write_mailbox(cpu_id, 1, argument);
    write_mailbox(cpu_id, 0, cached_entry);
    asm volatile("dbar 0" ::: "memory");
    send_ipi(cpu_id, 0);
}

void send_runtime_ipi(uint64 cpu_id)
{
    // runtime vector 与 TLB shootdown 共用硬件入口，但这里不发布 generation；
    // 中断处理只清 pending 位，不执行无请求的 invtlb。
    asm volatile("dbar 0" ::: "memory");
    send_ipi(cpu_id, k_runtime_ipi_vector);
}
}

namespace hal::smp
{
namespace
{
    [[noreturn]] void run_secondary(uint64 cpu_id)
    {
        Cpu::wait_for_bootstrap_ready();
        if (!Cpu::is_possible_cpu(cpu_id))
        {
            park_current_cpu();
        }

        // PGDL/PGDH、TLB、trap vector 和 timer 均为 CPU 本地状态。只有这些
        // 状态完整建立后才发布 online，主核随后才能安全放行调度器。
        mem::k_vmm.activate_kernel_pagetable();
        Cpu::initialize_current();
        hal::irq::initialize_current_cpu();
        trap_mgr.inithart();
        hal::tlb::initialize_current_cpu();
        Cpu::mark_current_online();
        if (Cpu::is_possible_cpu(cpu_id))
        {
            boardPrintfInfo("[smp] output: cpu=%lu online mask=0x%lx\n",
                            cpu_id, Cpu::online_cpu_mask());
        }
        else
        {
            boardPrintfWarn("[smp] output: late cpu=%lu initialized after timeout; parking\n",
                            cpu_id);
        }
        Cpu::wait_for_scheduler_ready();
        // 主核可能因启动超时收缩了 possible mask。已经越过第一道检查但尚未
        // 发布 online 的迟到次核，在进入调度器前必须再次确认自己仍被接纳。
        if (!Cpu::is_possible_cpu(cpu_id))
        {
            park_current_cpu();
        }
        proc::k_scheduler.start_schedule();
        park_current_cpu();
    }

    void request_secondary_start(uint64 boot_argument)
    {
        const uint64 bootstrap_cpu = Cpu::bootstrap_cpu_id();
        const uint64 possible_mask = Cpu::possible_cpu_mask();
        const uint64 entry = reinterpret_cast<uint64>(_secondary_entry);

        for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
        {
            if (cpu_id == bootstrap_cpu || (possible_mask & (1ULL << cpu_id)) == 0)
            {
                continue;
            }

            // 固件停驻代码从 mailbox0 读取入口，mailbox1 保留启动参数。
            boardPrintf("[smp] request: cpu=%lu entry-physical=0x%lx "
                        "entry-cached=0x%lx argument=0x%lx\n",
                        cpu_id, loongarch::board::physical_address(entry),
                        loongarch::board::cached_address(entry), boot_argument);
            loongarch::smp::start_secondary_cpu(cpu_id, entry, boot_argument);
        }
    }
}

void enter(uint64 cpu_id, uint64 boot_argument)
{
    (void)boot_argument;
    if (!Cpu::is_valid_cpu_id(cpu_id))
    {
        park_current_cpu();
    }
    if (!Cpu::try_claim_bootstrap())
    {
        run_secondary(cpu_id);
    }

    Cpu::bootstrap_begin();
}

void configure_topology()
{
    uint64 cpu_ids[NCPU] = {};
    int cpu_count = DtbManager::get_cpu_hartids(cpu_ids, NCPU);
    if (cpu_count == 0)
    {
        boardPrintfWarn("[smp] input: DTB has no usable CPU nodes; using boot CPU only\n");
        cpu_ids[0] = Cpu::bootstrap_cpu_id();
        cpu_count = 1;
    }

    Cpu::configure_topology(cpu_ids, cpu_count);
    boardPrintfInfo("[smp] input: boot-cpu=%lu dtb-cpus=%d possible-mask=0x%lx\n",
                    Cpu::bootstrap_cpu_id(), cpu_count, Cpu::possible_cpu_mask());
    for (int index = 0; index < cpu_count; ++index)
    {
        boardPrintf("[smp] input cpu[%d]=%lu\n", index, cpu_ids[index]);
    }
}

void start_secondaries(uint64 boot_argument)
{
    // LoongArch 次核初始停在固件停驻代码；先发 mailbox/IPI，再发布
    // bootstrap gate，可保证次核无论先后到达都只能观察到完整全局状态。
    hal::tlb::initialize_current_cpu();
    boardPrintfInfo("[smp] startup: possible=0x%lx already-online=0x%lx\n",
                    Cpu::possible_cpu_mask(), Cpu::online_cpu_mask());
    request_secondary_start(boot_argument);
    Cpu::publish_bootstrap_ready();

    // 固件没有停驻某个 DTB CPU、mailbox 契约不匹配或次核硬件故障时，不能
    // 永久阻塞主核。两秒后收缩到已上线 CPU，系统仍可安全以单核方式启动。
    const uint64 wait_start = rdtime();
    const uint64 wait_cycles = tmm::clock_frequency_hz() * 2ULL;
    while (Cpu::online_cpu_mask() != Cpu::possible_cpu_mask())
    {
        if (rdtime() - wait_start >= wait_cycles)
        {
            const uint64 missing = Cpu::retain_online_cpus_only();
            boardPrintfWarn("[smp] secondary startup timeout, disabled mask=0x%lx "
                            "online=0x%lx\n",
                            missing, Cpu::online_cpu_mask());
            break;
        }
        asm volatile("" ::: "memory");
    }
    boardPrintfInfo("[smp] output: possible=0x%lx online=0x%lx cpus=%d\n",
                    Cpu::possible_cpu_mask(), Cpu::online_cpu_mask(),
                    Cpu::online_cpu_count());
    Cpu::publish_scheduler_ready();
}

void kick_cpu(uint64 cpu_id)
{
    if (!Cpu::is_valid_cpu_id(cpu_id) ||
        cpu_id == Cpu::current_cpu_id() ||
        (Cpu::online_cpu_mask() & (1ULL << cpu_id)) == 0)
    {
        return;
    }
    loongarch::smp::send_runtime_ipi(cpu_id);
}

[[noreturn]] void park_current_cpu()
{
	// 被拓扑剔除的迟到次核已经可能打开了本地 timer。停驻前关闭中断，避免它
	// 在不属于调度拓扑的状态下继续进入公共 trap/tick 路径。
	Cpu::interrupt_off();
    for (;;)
    {
        asm volatile("nop");
    }
}
}

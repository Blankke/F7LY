#include "hal/smp.hh"

#include "devs/dtb.hh"
#include "hal/cpu.hh"
#include "hal/irq.hh"
#include "hal/riscv/sbi.hh"
#include "hal/tlb_shootdown.hh"
#include "mem/virtual_memory_manager.hh"
#include "printer.hh"
#include "proc/scheduler.hh"
#include "tm/time.hh"
#include "trap.hh"

extern "C" char _entry[];

namespace hal::smp
{
namespace
{
    constexpr uint64 k_secondary_start_timeout_us = 2'000'000ULL;

    [[noreturn]] void run_secondary(uint64 hart_id)
    {
        Cpu::wait_for_bootstrap_ready();
        if (!Cpu::is_possible_cpu(hart_id))
        {
            park_current_cpu();
        }

        // SATP、trap vector 和外部中断 context 均是 hart 本地状态。只有这些状态
        // 完整建立后才能发布 online，避免主核过早把用户任务投递到半初始化 CPU。
        mem::k_vmm.activate_kernel_pagetable();
        Cpu::initialize_current();
        hal::irq::initialize_current_cpu();
        trap_mgr.inithart();
        hal::tlb::initialize_current_cpu();
        Cpu::mark_current_online();
        Cpu::wait_for_scheduler_ready();

        // 主核可能在本 hart 初始化期间因超时收缩了 possible 集合。迟到 hart
        // 不能再进入调度器，否则调度拓扑和实际执行 CPU 会产生分歧。
        if (!Cpu::is_possible_cpu(hart_id))
        {
            park_current_cpu();
        }
        proc::k_scheduler.start_schedule();
        park_current_cpu();
    }

    void request_secondary_start(uint64 boot_argument)
    {
        const uint64 possible_mask = Cpu::possible_cpu_mask();
        const uint64 bootstrap_cpu = Cpu::bootstrap_cpu_id();

        for (uint64 hart_id = 0; hart_id < NCPU; ++hart_id)
        {
            if (hart_id == bootstrap_cpu || (possible_mask & (1ULL << hart_id)) == 0)
            {
                continue;
            }

            // OpenSBI HSM 将 stopped hart 带到统一的 _entry，opaque 会作为 a1
            // 返回。若固件已提前启动该 hart，它已经在 bootstrap gate 上等待。
            const int result = sbi_hart_start(
                hart_id,
                reinterpret_cast<uint64>(_entry),
                boot_argument);
            if (result != 0)
            {
                printfYellow("[smp] SBI hart_start hart=%lu ret=%d\n", hart_id, result);
            }
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

    // 从这里返回的只能是唯一主核。清空并发布启动状态的操作集中在 HAL，
    // boot/main 不再需要理解 Cpu 状态机的实现细节。
    Cpu::bootstrap_begin();
}

void configure_topology()
{
    uint64 hart_ids[NCPU] = {};
    int hart_count = DtbManager::get_cpu_hartids(hart_ids, NCPU);
    if (hart_count == 0)
    {
        // DTB 缺失 /cpus 时保留可诊断的单核启动路径。
        hart_ids[0] = Cpu::bootstrap_cpu_id();
        hart_count = 1;
    }

    Cpu::configure_topology(hart_ids, hart_count);
}

void start_secondaries(uint64 boot_argument)
{
    // 主核很早就被标记 online，但只有此时本地页表和中断入口已完整；先做
    // 最终 TLB 初始化，再允许次核依次发布 online。
    hal::tlb::initialize_current_cpu();
    request_secondary_start(boot_argument);
    Cpu::publish_bootstrap_ready();

    // 固件未实现 HSM、DTB 声明了不存在的 hart，或某个 hart 启动失败时，
    // 主核不能无限等待。超时时间由当前平台恒定计数器换算，避免把 QEMU 的
    // timebase 频率写死到架构通用代码；超时后只保留已完成初始化的 CPU。
    const uint64 wait_start = platform::clock_backend::read_ticks();
    const uint64 wait_ticks = tmm::microseconds_to_cycles(k_secondary_start_timeout_us);
    while (Cpu::online_cpu_mask() != Cpu::possible_cpu_mask())
    {
        if (platform::clock_backend::read_ticks() - wait_start >= wait_ticks)
        {
            const uint64 missing = Cpu::retain_online_cpus_only();
            // 这是启动硬件异常而非普通调试信息：仅在真正超时时输出一次，且
            // 绕过默认关闭的 printf 组，确保串口日志能解释为何降级为少核。
            k_printer.print_board_diagnostic(
                "[smp] secondary startup timeout, disabled mask=0x%lx online=0x%lx\n",
                missing, Cpu::online_cpu_mask());
            break;
        }
        asm volatile("" ::: "memory");
    }
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
    unsigned long hart_mask = 1UL << cpu_id;
    sbi_send_ipi(&hart_mask);
}

[[noreturn]] void park_current_cpu()
{
    // 已从 possible 集合剔除的迟到 hart 可能已经配置本地中断。停驻前关中断，
    // 避免它继续进入公共 trap/tick 路径。
    Cpu::interrupt_off();
    for (;;)
    {
        asm volatile("wfi");
    }
}
}

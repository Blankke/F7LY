#include "hal/smp.hh"

#include "devs/dtb.hh"
#include "hal/cpu.hh"
#include "hal/riscv/sbi.hh"
#include "hal/tlb_shootdown.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/scheduler.hh"
#include "trap.hh"
#include "trap/riscv/plic.hh"

extern "C" char _entry[];

namespace hal::smp
{
namespace
{
    [[noreturn]] void run_secondary(uint64 hart_id)
    {
        Cpu::wait_for_bootstrap_ready();
        if (!Cpu::is_possible_cpu(hart_id))
        {
            park_current_cpu();
        }

        // SATP、trap vector 和 PLIC context 均是 hart 本地状态。只有这些状态
        // 完整建立后才能发布 online，避免主核过早把用户任务投递到半初始化 CPU。
        mem::k_vmm.activate_kernel_pagetable();
        Cpu::initialize_current();
        trap_mgr.inithart();
        plic_mgr.inithart();
        hal::tlb::initialize_current_cpu();
        Cpu::mark_current_online();
        Cpu::wait_for_scheduler_ready();
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
    Cpu::wait_for_all_possible_cpus_online();
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
    for (;;)
    {
        asm volatile("wfi");
    }
}
}

#include "cpu.hh"
#include "tm/platform_clock_backend.hh"
#include <EASTL/atomic.h>
Cpu k_cpus[NUMCPU];

namespace
{
    // 启动阶段只有主核会写 possible mask；online mask 则由每个已完成本地
    // trap/scheduler 初始化的 CPU 自己置位。release/acquire 保证次核在看到
    // ready 之后不会读取到半初始化的全局对象。
    eastl::atomic<uint64> g_possible_cpu_mask{1};
    eastl::atomic<uint64> g_online_cpu_mask{0};
    eastl::atomic<uint32> g_bootstrap_state{0};
    eastl::atomic<uint32> g_scheduler_ready{0};
    eastl::atomic<uint64> g_bootstrap_cpu_id{~0ULL};

    constexpr uint32 k_bootstrap_initializing = 1;
    constexpr uint32 k_bootstrap_ready = 2;

    constexpr uint64 k_cpu_capacity_mask = (1ULL << NCPU) - 1;

    int count_cpu_bits(uint64 mask)
    {
        int count = 0;
        while (mask != 0)
        {
            count += static_cast<int>(mask & 1U);
            mask >>= 1;
        }
        return count;
    }
}

// ---- public:
Cpu *Cpu::get_cpu()
{
    uint64 x = current_cpu_id();

    if (!is_valid_cpu_id(x))
    {
        panic("Cpu::get_cpu: invalid cpu id=%lu capacity=%d", x, NCPU);
        return &k_cpus[0];
    }

    return &k_cpus[x];
}

bool Cpu::is_valid_cpu_id(uint64 cpu_id)
{
    return cpu_id < NCPU && cpu_id < 64;
}

uint64 Cpu::current_cpu_id()
{
#ifdef RISCV
    return read_tp();
#elif defined(LOONGARCH)
    // LoongArch 长跑里，tp 在 userret/信号返回等极窄窗口可能暂时带着用户 TLS。
    // 当前 CPU 槽位必须直接按 CSR_CPUID 取，避免把这个瞬时寄存器态扩散到
    // get_cur_pcb()/锁 owner/中断嵌套计数等通用路径上。
    return r_csr_cpuid();
#endif
}

void Cpu::initialize_current()
{
    Cpu *cpu = get_cpu();
    cpu->_cur_proc = nullptr;
    cpu->_num_off = 0;
    cpu->_int_ena = 0;
    cpu->_timeslice_ticks = 0;
}

bool Cpu::try_claim_bootstrap()
{
    uint64 expected = ~0ULL;
    return g_bootstrap_cpu_id.compare_exchange_strong(expected,
                                                      current_cpu_id(),
                                                      eastl::memory_order_acq_rel);
}

void Cpu::bootstrap_begin()
{
    g_bootstrap_state.store(k_bootstrap_initializing, eastl::memory_order_release);
    g_online_cpu_mask.store(0, eastl::memory_order_release);
    g_scheduler_ready.store(0, eastl::memory_order_release);
    initialize_current();
    mark_current_online();
}

void Cpu::configure_topology(const uint64 *hartids, int hart_count)
{
    uint64 mask = 0;

    for (int index = 0; hartids != nullptr && index < hart_count; ++index)
    {
        const uint64 hartid = hartids[index];
        if (is_valid_cpu_id(hartid))
        {
            mask |= 1ULL << hartid;
        }
    }

    // OpenSBI/QEMU 不保证 boot hart 必然是 CPU0。即使 DTB 异常或遗漏该节点，
    // 已经认领主核的 CPU 也必须保留在可用拓扑中，才能继续单核启动。
    const uint64 bootstrap_cpu = bootstrap_cpu_id();
    if (is_valid_cpu_id(bootstrap_cpu))
    {
        mask |= 1ULL << bootstrap_cpu;
    }
    if (mask == 0)
    {
        mask = 1;
    }
    g_possible_cpu_mask.store(mask & k_cpu_capacity_mask, eastl::memory_order_release);
}

void Cpu::publish_bootstrap_ready()
{
    g_bootstrap_state.store(k_bootstrap_ready, eastl::memory_order_release);
}

void Cpu::wait_for_bootstrap_ready()
{
    while (!is_bootstrap_ready())
    {
        asm volatile("" ::: "memory");
    }
}

bool Cpu::is_bootstrap_ready()
{
    return g_bootstrap_state.load(eastl::memory_order_acquire) == k_bootstrap_ready;
}

void Cpu::publish_scheduler_ready()
{
    // release 与次核 wait_for_scheduler_ready() 的 acquire 配对，保证次核在
    // 进入调度器前能观察到其它 CPU 的 online 位及完整的全局初始化结果。
    g_scheduler_ready.store(1, eastl::memory_order_release);
}

void Cpu::wait_for_scheduler_ready()
{
    while (g_scheduler_ready.load(eastl::memory_order_acquire) == 0)
    {
        asm volatile("" ::: "memory");
    }
}

uint64 Cpu::bootstrap_cpu_id()
{
    return g_bootstrap_cpu_id.load(eastl::memory_order_acquire);
}

bool Cpu::is_bootstrap_cpu()
{
    return current_cpu_id() == bootstrap_cpu_id();
}

bool Cpu::is_possible_cpu(uint64 cpu_id)
{
    return is_valid_cpu_id(cpu_id) &&
           (possible_cpu_mask() & (1ULL << cpu_id)) != 0;
}

void Cpu::mark_current_online()
{
    const uint64 cpu_id = current_cpu_id();
    if (!is_valid_cpu_id(cpu_id))
    {
        return;
    }
    g_online_cpu_mask.fetch_or(1ULL << cpu_id, eastl::memory_order_release);
}

uint64 Cpu::possible_cpu_mask()
{
    return g_possible_cpu_mask.load(eastl::memory_order_acquire) & k_cpu_capacity_mask;
}

uint64 Cpu::online_cpu_mask()
{
    return g_online_cpu_mask.load(eastl::memory_order_acquire) & possible_cpu_mask();
}

uint64 Cpu::retain_online_cpus_only()
{
    const uint64 old_possible = possible_cpu_mask();
    uint64 retained = g_online_cpu_mask.load(eastl::memory_order_acquire) &
                      k_cpu_capacity_mask;
    const uint64 bootstrap_cpu = bootstrap_cpu_id();
    if (is_valid_cpu_id(bootstrap_cpu))
    {
        retained |= 1ULL << bootstrap_cpu;
    }
    if (retained == 0)
    {
        retained = 1;
    }
    g_possible_cpu_mask.store(retained, eastl::memory_order_release);
    return old_possible & ~retained;
}

int Cpu::possible_cpu_count()
{
    return count_cpu_bits(possible_cpu_mask());
}

int Cpu::online_cpu_count()
{
    return count_cpu_bits(online_cpu_mask());
}

bool Cpu::advance_time_slice(uint32 limit)
{
    if (limit == 0)
    {
        return false;
    }

    ++_timeslice_ticks;
    if (_timeslice_ticks < limit)
    {
        return false;
    }

    _timeslice_ticks = 0;
    return true;
}

uint64 Cpu::get_time()
{
	return platform::clock_backend::read_ticks();
}

void Cpu::push_intr_off()
{
	int old = get_intr_stat();

	_intr_off();
	Cpu *c_cpu = get_cpu();
	if (c_cpu->_num_off == 0)
		c_cpu->_int_ena = old;
	c_cpu->_num_off += 1;
}

void Cpu::pop_intr_off()
{
	Cpu *c_cpu = get_cpu();
	if (get_intr_stat())
		panic("pop intr off - interruptible");
	if (c_cpu->_num_off < 1)
		panic("pop intr off - none to pop off");
	c_cpu->_num_off -= 1;
	if (c_cpu->_num_off == 0 && c_cpu->_int_ena)
		_intr_on();
}

void Cpu::reset_intr_off_depth()
{
	_num_off = 0;
	_int_ena = 0;
}

// RISC-V 中类似的操作是使能浮点单元（FPU），通常通过设置 mstatus 寄存器的 FS (Floating-point Status) 位。
// 例如，可以这样实现：

void Cpu::enable_fpu()
{
#ifdef RISCV
	uint64 mstatus = r_mstatus();
	// 设置 FS 字段为 0b11（Dirty），允许使用浮点指令
	mstatus |= (0x3UL << 13); // FS 位在 mstatus[14:13]
	w_mstatus(mstatus);
#elif defined(LOONGARCH)
	uint64 tmp = r_csr_euen();
	w_csr_euen(tmp | 1);
#endif
}

// ---- private:

// uint64 Cpu::read_csr(csr::CsrAddr r)
// {
// 	return csr::_read_csr_(r);
// }

// void Cpu::write_csr(csr::CsrAddr r, uint64 d)
// {
// 	return csr::_write_csr_(r, d);
// }

void Cpu::_intr_on()
{
#ifdef RISCV
	w_sstatus(r_sstatus() | SSTATUS_SIE);
#elif defined(LOONGARCH)
	uint64 tmp = r_csr_crmd();
	w_csr_crmd(tmp | loongarch::csr::crmd::ie_m);
#endif
}

void Cpu::_intr_off()
{
#ifdef RISCV
	w_sstatus(r_sstatus() & ~SSTATUS_SIE);
#elif defined(LOONGARCH)
	uint64 tmp = r_csr_crmd();
	w_csr_crmd(tmp & ~loongarch::csr::crmd::ie_m);
#endif
}

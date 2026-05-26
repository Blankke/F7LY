#include "cpu.hh"
#ifdef RISCV
#include "rv_csr.hh"
#elif defined(LOONGARCH)
#include "la_csr.hh"
#endif
Cpu k_cpus[NUMCPU];

// ---- public:
Cpu *Cpu::get_cpu()
{
	uint64 x;
#ifdef RISCV
	x = read_tp();
#elif defined(LOONGARCH)
	// LoongArch 长跑里，tp 在 userret/信号返回等极窄窗口可能暂时带着用户 TLS。
	// 当前 CPU 槽位必须直接按 CSR_CPUID 取，避免把这个瞬时寄存器态扩散到
	// get_cur_pcb()/锁 owner/中断嵌套计数等通用路径上。
	x = r_csr_cpuid();
#endif
	return &k_cpus[x];
}

uint64 Cpu::get_time()
{
	#ifdef RISCV
	return r_time();
	#elif defined (LOONGARCH)
	return r_csr_tval();
	//loongarch 不需要这个函数
	#endif
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

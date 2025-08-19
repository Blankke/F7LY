

#include "spinlock.hh"
#include "cpu.hh"
#include "printer.hh"

SpinLock::SpinLock()
{
}

void SpinLock::init(const char *name)
{
	_name = name;
	_locked = nullptr;
}

#define LOCK 
void SpinLock::acquire()
{
#ifndef LOCK
	Cpu *cpu = Cpu::get_cpu();
	cpu->push_intr_off();

	if (is_held())
	{
		printf("lock name %s is already held\n", _name);
		panic("spinlock acquire");
	}

	eastl::atomic_thread_fence(eastl::memory_order_acq_rel);

	Cpu *expected = nullptr;
	while (_locked.compare_exchange_strong(expected, cpu, eastl::memory_order_acq_rel) == false)
		expected = nullptr;
#endif
}

void SpinLock::release()
{
#ifndef LOCK
	if (!is_held())
	{
		printf("lock name %s is already released\n", _name);
		panic("spinlock released.");
	}
	// _locked.store( nullptr, eastl::memory_order_acq_rel );
	Cpu *cpu = Cpu::get_cpu();
	_locked.store(nullptr);

	eastl::atomic_thread_fence(eastl::memory_order_acq_rel);
	cpu->pop_intr_off();
#endif
}

bool SpinLock::is_held()
{
	return (_locked.load() == Cpu::get_cpu());
}

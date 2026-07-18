

#include "spinlock.hh"
#include "cpu.hh"
#include "printer.hh"


	SpinLock::SpinLock()
	{

	}

	void SpinLock::init( const char * name )
	{
		_name = name;
		_locked = nullptr;
	}

	void SpinLock::acquire()
	{
		// 先屏蔽本地中断，再读取 Cpu 槽位。若反过来，计时中断可能在两者之间
		// 把当前任务迁移到另一核，最终把锁错误地标记为旧 CPU 持有。
		Cpu::push_intr_off();
		Cpu * cpu = Cpu::get_cpu();

		if ( is_held() ){
			panic( "spinlock acquire: lock=%s cpu=%p", _name ? _name : "(unnamed)", cpu );
		}
		
		eastl::atomic_thread_fence( eastl::memory_order_acq_rel );

		Cpu * expected = nullptr;
		while ( _locked.compare_exchange_strong( expected, cpu, eastl::memory_order_acq_rel ) == false )
			expected = nullptr;
	}

	bool SpinLock::try_acquire()
	{
		// 与 acquire() 保持同一不变量：拿到当前 CPU 指针前不允许发生迁移。
		Cpu::push_intr_off();
		Cpu *cpu = Cpu::get_cpu();

		if (is_held())
		{
			panic("spinlock try_acquire: lock=%s cpu=%p", _name ? _name : "(unnamed)", cpu);
		}

		Cpu *expected = nullptr;
		if (_locked.compare_exchange_strong(expected, cpu, eastl::memory_order_acq_rel))
		{
			return true;
		}

		// try_acquire 对失败路径也必须还原本 CPU 的关中断嵌套，不能让调度器
		// 因连续跳过远端运行任务而永久关闭中断。
		Cpu::pop_intr_off();
		return false;
	}

	void SpinLock::release()
	{
		if ( !is_held() ){
			panic( "spinlock released: lock=%s cpu=%p owner=%p", _name ? _name : "(unnamed)", Cpu::get_cpu(), _locked.load() );
		}
		// _locked.store( nullptr, eastl::memory_order_acq_rel );
		Cpu * cpu = Cpu::get_cpu();
		_locked.store( nullptr );

		eastl::atomic_thread_fence( eastl::memory_order_acq_rel );
		cpu->pop_intr_off();
	}

	bool SpinLock::is_held()
	{
		return ( _locked.load() == Cpu::get_cpu() );
	}

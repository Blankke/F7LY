#include "hal/cpu.hh"
#include "spinlock.hh"
#include "scheduler.hh"
#include "proc_manager.hh"
#include "printer.hh"
#include "pagetable.hh"
// #include "tm/timer_manager.hh"
// #include "klib/common.hh"

extern "C" {
	extern void swtch( proc::Context *to_store, proc::Context *to_switch );
}

namespace proc
{

	Scheduler k_scheduler;

	void Scheduler::init( const char *name )
	{ 
		_sche_lock.init( name );
	}

	int Scheduler::get_highest_proirity()
	{
		_sche_lock.acquire();
		int prio = lowest_proc_prio;
		for ( Pcb &p : k_proc_pool )
		{
			if (p._priority < prio && p._state == ProcState::RUNNABLE)
			{
				prio = p._priority;
			}
		}
		_sche_lock.release();
		return prio;
	}

	void Scheduler::start_schedule()
	{
		Pcb *p;
		Cpu *cpu = Cpu::get_cpu();
		int priority;

		cpu->set_cur_proc( nullptr );

		for ( ;;)
		{
			
			cpu->interrupt_on();

			priority = get_highest_proirity();

			for ( p = k_proc_pool; p < &k_proc_pool[ num_process ]; p++ )
			{
				if (p->_state != ProcState::RUNNABLE || p->_priority > priority)
				{
					continue;
				}

				p->_lock.acquire();
				p->print_context();
				if (p->get_state() == ProcState::RUNNABLE)
				{
					p->_state = ProcState::RUNNING;
					cpu->set_cur_proc( p );
					proc::Context* cur_context = cpu->get_context();
					print_context1( cur_context );

					//Debug
					uint64 sp = 0x0000001ffffbf000;
					uint64 pa = (uint64)mem::k_pagetable.walk_addr(sp);
					printf("sp: %p, pa: %p\n", sp, pa);


					// printf( "sche proc %d, name: %s\n", p->_gid, p->_name );
					swtch( cur_context, &p->_context );
					// printf( "return from %d, name: %s\n", p->_gid, p->_name );
					cpu->set_cur_proc( nullptr );
				}
				p->_lock.release();
				break;
			}
		}
	}

	void Scheduler::yield()
	{
		Pcb *p = Cpu::get_cpu()->get_cur_proc();
		p->_lock.acquire();
		p->_state = ProcState::RUNNABLE;
		call_sched();
		p->_lock.release();

	}

	void Scheduler::call_sched()
	{
		int intena;
		Cpu * cpu = Cpu::get_cpu();
		Pcb *p = cpu->get_cur_proc();

		assert( p->_lock.is_held(), "sched: proc lock not held" );
		assert( cpu->get_num_off() == 1, "sched: proc locks" );
		assert(p->_state != ProcState::RUNNING, "sched: proc is running");
		assert( cpu->get_intr_stat() == false, "sched: interruptible" );

		intena = cpu->get_int_ena();
		swtch( &p->_context, cpu->get_context() );
		cpu->set_int_ena( intena );
	}

}
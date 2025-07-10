#pragma once
#include "spinlock.hh"
#include "proc.hh"

namespace proc
{

	class Scheduler
	{
	private:
		SpinLock _sche_lock;
	public:
		Scheduler() = default;
		void init( const char *name );
		void add_thread();
		void remove_thread();
		void switch_to_proc(Pcb *p);
		int  get_highest_proirity();
		void start_schedule();
		// void switch_to_proc( pm::Pcb *p );

		void yield();
		void call_sched();
	};

	extern Scheduler k_scheduler;
}
#pragma once
#include "spinlock.hh"
#include "proc.hh"

namespace proc
{

	class Scheduler
	{
	private:
		SpinLock _sche_lock;
		bool _has_non_default_priority = false;
		// F7LY 当前采用全局进程池而非 per-CPU run queue。每核维护独立的扫描
		// 游标，避免所有空闲核总从 slot 0 竞争；任务筛选再结合 _last_cpu 的
		// 粘附放置，形成与 Starry run-queue locality 等价的默认不迁移语义。
		uint _next_scan_global_id[NUMCPU] = {};
		// 新建 runnable 任务在首次运行前没有“最近 CPU”。以轮转游标为它们
		// 选择初始 home CPU，避免 pthread 等批量 clone 的任务都先挤到父线程
		// 所在核；后续由 _last_cpu 粘附策略维持局部性。
		int _next_initial_cpu = -1;
	public:
		Scheduler() = default;
		void init( const char *name );
		void note_priority_change(int priority);
		int select_initial_cpu(const CpuMask &mask, int parent_cpu);
		void add_thread();
		void remove_thread();
		void switch_to_proc(Pcb *p);
		int  get_highest_priority(int cpu_id);
		void start_schedule();
		// void switch_to_proc( pm::Pcb *p );

		void yield();
		void call_sched();
	};

	extern Scheduler k_scheduler;
}

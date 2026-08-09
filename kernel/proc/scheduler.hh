#pragma once
#include "spinlock.hh"
#include "proc.hh"
#include <EASTL/atomic.h>

namespace proc
{

	class Scheduler
	{
	private:
		struct alignas(64) CpuPressureCounter
		{
			// RUNNABLE 和 RUNNING 都占用一份调度容量；两者之间的
			// 每个时间片切换不应该触发原子 RMW。
			eastl::atomic<uint32> schedulable{0};
		};

		SpinLock _sche_lock;
		// 普通 BuildStorm 负载没有实时优先级任务。这个标志若使用普通 bool，
		// 每个空闲 CPU 仍会在每轮 512 项扫描前争用全局调度锁。
		// 原子快路径让默认优先级场景完全绕开这把锁；慢路径仍由 _sche_lock
		// 串行扫描和清理标志。
		// 精确记录当前可调度的非默认优先级任务。旧的布尔标志在 SMP 下无法
		// 安全清零：只要其它 CPU 正在运行默认任务，慢路径就拿不到其 PCB 锁，
		// 最终让所有 scheduler 永久争用全局锁并扫描活跃任务。
		eastl::atomic<uint32> _non_default_schedulable{0};
		// PCB 生命周期和诊断遍历仍由全局进程池管理；调度热路径只读取每核
		// runnable 位图，避免每个空闲核反复扫描全局 active PCB 表。
		static constexpr uint k_runnable_slot_word_count = (num_process + 63) / 64;
		eastl::atomic<uint64> _runnable_slot_words[NUMCPU][k_runnable_slot_word_count]{};
		uint _next_scan_global_id[NUMCPU] = {};
		// 新建 runnable 任务在首次运行前没有“最近 CPU”。游标只负责相同
		// 负载时的公平破局；实际 home CPU 会按当前活跃任务压力选择。
		int _next_initial_cpu = -1;
		// 只记录真正占用调度容量的 RUNNABLE/RUNNING 任务。每核
		// 计数独占缓存行，避免 8 个 CPU 在 clone/wakeup/exit 时伪共享。
		// 所有运行期状态和 home CPU 变更都必须经过下面两个入口，
		// 因此 clone 初始选核只需读取 NUMCPU 个原子计数。
		CpuPressureCounter _pressure[NUMCPU] = {};
		uint64 _load_averages[3] = {};
		uint64 _load_last_sample_sec = 0;
		bool _load_initialized = false;
	public:
		Scheduler() = default;
		void init( const char *name );
		void set_task_priority(Pcb &task, int priority);
		int select_initial_cpu(const CpuMask &mask, int parent_cpu);
		void set_task_state(Pcb &task, ProcState state);
		void set_task_home_cpu(Pcb &task, int cpu_id);
		uint64 runnable_slot_word(uint cpu_id, uint word_index) const;
		uint32 runnable_task_count() const;
		// CPU0 每 5 秒从时钟中断采样一次，sysinfo 只读取已经形成的历史值。
		void sample_load_averages(uint64 now_sec);
		void snapshot_load_averages(uint64 loads[3]);
		void add_thread();
		void remove_thread();
		void switch_to_proc(Pcb *p);
		int  get_highest_priority(int cpu_id);
		void start_schedule();
		// void switch_to_proc( pm::Pcb *p );

		void yield();
		void call_sched();

	private:
		void mark_runnable_slot(uint cpu_id, uint global_id);
		void clear_runnable_slot(uint cpu_id, uint global_id);
	};

	extern Scheduler k_scheduler;
}

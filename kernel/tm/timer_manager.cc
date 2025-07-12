//
// Copied from Li shuang ( pseudonym ) on 2024-04-05
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#include "tm/timer_manager.hh"
#include "proc/proc_manager.hh"
#include "klib.hh"
#include "trap/riscv/trap.hh"
#include "timer_interface.hh"

namespace tmm
{
	TimerManager k_tm;

	void TimerManager::init(const char *lock_name)
	{
		_lock.init(lock_name);

		trap_mgr.ticks = 0;
		printfGreen("[TM] Timer Manager Init\n");
		// close_ti_intr();
	}

	// void TimerManager::open_ti_intr()
	// {
	// 	_lock.acquire();
	// 	_tcfg_data |= ( loongarch::csr::Tcfg::tcfg_en_m );
	// 	loongarch::Cpu::write_csr( loongarch::csr::CsrAddr::tcfg, _tcfg_data );
	// 	_lock.release();
	// }

	// void TimerManager::close_ti_intr()
	// {
	// 	_lock.acquire();
	// 	_tcfg_data &= ~( loongarch::csr::Tcfg::tcfg_en_m );
	// 	loongarch::Cpu::write_csr( loongarch::csr::CsrAddr::tcfg, _tcfg_data );
	// 	_lock.release();
	// }

	// int TimerManager::handle_clock_intr()
	// {
	// 	_lock.acquire();
	// 	trap_mgr.ticks++;
	// 	// printf( "t" );
	// 	// loongarch::Cpu::write_csr( loongarch::csr::CsrAddr::tcfg, _tcfg_data );
	// 	proc::k_pm.wakeup(&trap_mgr.ticks);
	// 	_lock.release();
	// 	return 0;
	// }

	timeval TimerManager::get_time_val()
	{
		uint64 t_val;
		// uint64 cpt = tmm::cycles_per_tick();

		_lock.acquire();
		t_val = tmm::get_hw_time_stamp();
		// t_val += trap_mgr.ticks * cpt;
		_lock.release();

		timeval tv;
		tv.tv_sec = t_val / tmm::get_main_frequence();
		tv.tv_usec = t_val % tmm::get_main_frequence();
		tv.tv_usec = tmm::time_stamp_to_usec(tv.tv_usec);

		// tv.tv_sec = trap_mgr.trap_mgr.ticks * ms_per_tick / 1000;
		// tv.tv_usec = ( ( trap_mgr.trap_mgr.ticks * ms_per_tick ) % 1000 ) * 1000;

		// Info("invoke get time = %d : %d", tv.tv_sec, tv.tv_usec);
		return tv;
	}

	int TimerManager::sleep_n_ticks(int n)
	{
		if (n < 0)
			return -1;

		uint64 tick_tmp;
		proc::Pcb *p = proc::k_pm.get_cur_pcb();

		_lock.acquire();
		tick_tmp = trap_mgr.ticks;
		while ((int)trap_mgr.ticks - (int)tick_tmp < (int)n)
		{
			// printfGreen("ticks now:%d,ticks left:%d\n",(int)trap_mgr.ticks,(int)tick_tmp);
			if (p->is_killed())
			{
				_lock.release();
				return -2;
			}
			proc::k_pm.sleep(&trap_mgr.ticks, &_lock);
		}
		_lock.release();

		return 0;
	}

	/// @brief  将 timeval 转换为系统时钟的睡眠时间
	/// @param tv  timeval 结构体，包含秒和微秒
	/// @return 
	int TimerManager::sleep_from_tv(timeval tv)
	{

		uint64 n = tv.tv_sec * tmm::get_main_frequence();
		uint64 cpt = tmm::cycles_per_tick();
		// printfBlue("sleep from tv: %u ticks\n", n);
		n += tmm::usec_to_time_stamp(tv.tv_usec);
		// printfBlue("sleep from tv: %u ticks\n", n);
		n /= cpt;
		// printfBlue("sleep from tv: %u ticks\n", n);
		if (n == 0)
			return 0; // 如果转换结果为0，直接返回
		return sleep_n_ticks(n);
	}

	int TimerManager::clock_gettime(SystemClockId cid, timespec *tp)
	{
		if (tp == nullptr)
			return -1; // 应该返回错误而不是0

		uint64 t_val;
		uint64 cpt = tmm::cycles_per_tick();
		uint64 freq = tmm::get_main_frequence();

		_lock.acquire();
		t_val = tmm::get_hw_time_stamp();
		t_val += trap_mgr.ticks * cpt;
		_lock.release();

		tp->tv_sec = (long)(t_val / freq);
		ulong rest_cyc = t_val % freq;

		// 使用整数运算: rest_cyc * 1,000,000,000 / freq
		const int64 nsec_max = 1e9;

		tp->tv_nsec = (long)((rest_cyc * nsec_max) / freq);

		while (tp->tv_nsec >= (long)nsec_max)
		{
			tp->tv_sec++;
			tp->tv_nsec -= (long)nsec_max;
		}
		printfBlue("tp->tv_sec: %d, tp->tv_nsec: %d\n", tp->tv_sec, tp->tv_nsec);
		while (tp->tv_nsec < 0)
		{
			tp->tv_sec--;
			tp->tv_nsec += nsec_max;
		}

		return 0;
	}
 uint64 TimerManager::get_ticks() { return trap_mgr.ticks; };
	extern "C"
	{

		int clock_gettime(clockid_t clk, struct timespec *tp)
		{
			return k_tm.clock_gettime((SystemClockId)clk, tp);
		}

		TODO("这个是没有实现的，后续进行补充")
		int *__errno_location(void)
		{
			// 如果你没有线程环境，可以直接返回全局 errno
			static int err;
			return &err;
		}
	}

} // namespace tmm

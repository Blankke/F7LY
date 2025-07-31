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

	/// @brief 初始化定时器管理器
	/// @param lock_name 锁的名称，用于调试和标识
	void TimerManager::init(const char *lock_name)
	{
		// 初始化定时器管理器的锁，用于保护并发访问
		_lock.init(lock_name);

		// 初始化系统tick计数器为0
		// tick是系统时间的基本单位，由硬件定时器中断驱动
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

	/// @brief 获取当前系统时间值（timeval格式）
	/// @return 返回包含秒和微秒的timeval结构体
	/// @note 主要用于gettimeofday系统调用
	timeval TimerManager::get_time_val()
	{
		uint64 t_val;
		// uint64 cpt = tmm::cycles_per_tick(); // 暂时不使用tick计算

		// 获取硬件时间戳（原子操作，需要加锁保护）
		_lock.acquire();
		t_val = tmm::get_hw_time_stamp();
		// t_val += trap_mgr.ticks * cpt; // 暂时不加上tick偏移
		_lock.release();

		timeval tv;
		// 转换为秒和微秒
		tv.tv_sec = t_val / tmm::get_main_frequence();
		tv.tv_usec = t_val % tmm::get_main_frequence();
		// 将剩余周期转换为微秒
		tv.tv_usec = tmm::time_stamp_to_usec(tv.tv_usec);

		// 备用计算方法（基于tick的毫秒计算）：
		// tv.tv_sec = trap_mgr.trap_mgr.ticks * ms_per_tick / 1000;
		// tv.tv_usec = ( ( trap_mgr.trap_mgr.ticks * ms_per_tick ) % 1000 ) * 1000;

		// Info("invoke get time = %d : %d", tv.tv_sec, tv.tv_usec);
		return tv;
	}

	/// @brief 使进程休眠指定的tick数
	/// @param n 要休眠的tick数，必须为非负数
	/// @return 成功返回0，被杀死返回-2，参数错误返回-1
	/// @note 这是一个可中断的睡眠，如果进程被标记为killed会提前返回
	int TimerManager::sleep_n_ticks(int n)
	{
		if (n < 0)
			return -1; 

		uint64 tick_tmp;
		proc::Pcb *p = proc::k_pm.get_cur_pcb(); // 获取当前进程控制块

		_lock.acquire();
		tick_tmp = trap_mgr.ticks; // 记录开始时的tick值
		
		// 循环等待直到经过了n个tick
		while ((int)trap_mgr.ticks - (int)tick_tmp < (int)n)
		{
			// printfGreen("ticks now:%d,ticks left:%d\n",(int)trap_mgr.ticks,(int)tick_tmp);
			
			// 检查进程是否被杀死
			if (p->is_killed())
			{
				_lock.release();
				return -2; 
			}
			
			// 进入睡眠状态，等待tick更新时被唤醒
			// 当定时器中断发生时，会调用wakeup(&trap_mgr.ticks)来唤醒等待的进程
			proc::k_pm.sleep(&trap_mgr.ticks, &_lock);
		}
		_lock.release();

		return 0;
	}

	/// @brief 根据timeval结构体指定的时间进行睡眠
	/// @param tv 包含睡眠时间的timeval结构体（秒+微秒）
	/// @return 成功返回0，失败返回负数
	/// @note 将timeval转换为tick数，然后调用sleep_n_ticks进行实际睡眠
	int TimerManager::sleep_from_tv(timeval tv)
	{
		// 将秒转换为周期数
		uint64 n = tv.tv_sec * tmm::get_main_frequence();
		uint64 cpt = tmm::cycles_per_tick(); // 每个tick的周期数
		
		// printfBlue("sleep from tv: %u ticks\n", n);
		
		// 将微秒转换为周期数并累加
		n += tmm::usec_to_time_stamp(tv.tv_usec);
		// printfBlue("sleep from tv: %u ticks\n", n);
		
		// 将总周期数转换为tick数
		n /= cpt;
		// printfBlue("sleep from tv: %u ticks\n", n);
		
		if (n == 0)
			return 0; // 如果转换结果为0，直接返回（无需睡眠）
			
		return sleep_n_ticks(n); // 执行实际的tick睡眠
	}

	/// @brief 获取指定时钟的当前时间
	/// @param cid 时钟类型ID，支持实时时钟、单调时钟、进程CPU时间等
	/// @param tp 输出参数，存储获取的时间值
	/// @return 成功返回0，失败返回负数错误码
	int TimerManager::clock_gettime(SystemClockId cid, timespec *tp)
	{
		if (tp == nullptr)
			return -1; // 无效的输出指针

		uint64 t_val;
		uint64 cpt = tmm::cycles_per_tick();  // 每个tick的周期数
		uint64 freq = tmm::get_main_frequence(); // 主频率

		// 根据不同的时钟类型获取相应的时间
		switch (cid)
		{
			case CLOCK_REALTIME: // 系统实时时钟（墙上时钟时间）
			case CLOCK_REALTIME_COARSE: // 粗粒度实时时钟
			{
				// 获取硬件时间戳并加上tick计数
				_lock.acquire();
				t_val = tmm::get_hw_time_stamp();
				t_val += trap_mgr.ticks * cpt;
				_lock.release();

				// 转换为秒和纳秒
				tp->tv_sec = (long)(t_val / freq);
				ulong rest_cyc = t_val % freq;

				// 计算纳秒部分：rest_cyc * 1,000,000,000 / freq
				const int64 nsec_max = 1000000000L;
				tp->tv_nsec = (long)((rest_cyc * nsec_max) / freq);

				// 处理纳秒溢出（确保纳秒值在有效范围内）
				while (tp->tv_nsec >= (long)nsec_max)
				{
					tp->tv_sec++;
					tp->tv_nsec -= (long)nsec_max;
				}
				
				// 处理纳秒为负数的情况
				while (tp->tv_nsec < 0)
				{
					tp->tv_sec--;
					tp->tv_nsec += nsec_max;
				}
				break;
			}
			
			case CLOCK_MONOTONIC: // 单调时钟（系统启动后的时间）
			case CLOCK_MONOTONIC_RAW: // 原始单调时钟
			case CLOCK_MONOTONIC_COARSE: // 粗粒度单调时钟
			case CLOCK_BOOTTIME: // 启动时间时钟
			{
				// 基于系统tick计数获取单调时间
				_lock.acquire();
				uint64 ticks = trap_mgr.ticks;
				_lock.release();
				
				uint64 total_cycles = ticks * cpt;
				tp->tv_sec = (long)(total_cycles / freq);
				ulong rest_cyc = total_cycles % freq;
				const int64 nsec_max = 1000000000L;
				tp->tv_nsec = (long)((rest_cyc * nsec_max) / freq);
				break;
			}
			
			case CLOCK_PROCESS_CPUTIME_ID: // 进程CPU时间
			case CLOCK_THREAD_CPUTIME_ID: // 线程CPU时间
			{
				// 获取当前进程的CPU使用时间
				proc::Pcb *p = proc::k_pm.get_cur_pcb();
				uint64 user_ticks = p->get_user_ticks(); // 用户态tick数
				uint64 stime = p->get_stime(); // 系统态时间（微秒）
				
				// 用户态时间转换：ticks -> cycles -> 时间
				uint64 user_time_cycles = user_ticks * cpt;
				uint64 user_time_sec = user_time_cycles / freq;
				uint64 user_time_nsec = ((user_time_cycles % freq) * 1000000000L) / freq;
				
				// 系统态时间转换：微秒 -> 秒和纳秒
				uint64 total_sec = user_time_sec + (stime / 1000000);
				uint64 total_nsec = user_time_nsec + ((stime % 1000000) * 1000);
				
				// 处理纳秒溢出
				if (total_nsec >= 1000000000L)
				{
					total_sec += total_nsec / 1000000000L;
					total_nsec %= 1000000000L;
				}
				
				tp->tv_sec = (long)total_sec;
				tp->tv_nsec = (long)total_nsec;
				break;
			}
			
			default:
			{
				// 不支持的时钟类型
				return -22; // -EINVAL
			}
		}

		// 调试输出时间值
		// printfYellow("clock_gettime: cid=%d, tp->tv_sec=%d, tp->tv_nsec=%d\n", 
		// 		  (int)cid, tp->tv_sec, tp->tv_nsec);

		return 0;
	}
	/// @brief 获取当前系统tick计数
	/// @return 返回从系统启动以来的tick数
	/// @note tick是系统时间的基本单位，由定时器中断驱动递增
	uint64 TimerManager::get_ticks() { return trap_mgr.ticks; };

	/// @brief 获取指定时钟的当前时间（仅秒数部分）
	/// @param clockid 时钟类型ID
	/// @return 成功返回秒数，失败返回-1
	/// @note 便于需要整数秒时间戳的场景，如文件时间戳
	int TimerManager::clock_gettime_sec(SystemClockId clockid)
	{
		timespec ts;
		int ret = clock_gettime(clockid, &ts);
		if (ret == 0) {
			return (int)ts.tv_sec;  // 返回秒数部分
		}
		return -1;  // 错误时返回-1
	}

	/// @brief 获取指定时钟的当前时间（仅纳秒数部分）
	/// @param clockid 时钟类型ID  
	/// @return 成功返回纳秒数，失败返回-1
	/// @note 返回当前秒内的纳秒偏移量 [0, 999999999]
	int TimerManager::clock_gettime_nsec(SystemClockId clockid)
	{
		timespec ts;
		int ret = clock_gettime(clockid, &ts);
		if (ret == 0) {
			return (int)ts.tv_nsec;  // 返回纳秒数部分
		}
		return -1;  // 错误时返回-1
	}
	
	// 导出的C接口函数，供C代码调用
	extern "C"
	{
		/// @brief C接口的clock_gettime函数
		/// @param clk 时钟ID，对应POSIX标准的clockid_t类型
		/// @param tp 输出参数，存储时间值的timespec结构体指针
		/// @return 成功返回0，失败返回负数
		/// @note 这是POSIX标准的clock_gettime函数的内核实现
		int clock_gettime(clockid_t clk, struct timespec *tp)
		{
			return k_tm.clock_gettime((SystemClockId)clk, tp);
		}

		/// @brief 获取错误号存储位置的指针
		/// @return 返回指向错误号变量的指针
		/// @note 这是标准C库的__errno_location函数实现
		/// @todo 这个是临时实现，后续需要支持线程局部存储
		int *__errno_location(void)
		{
			// 如果你没有线程环境，可以直接返回全局 errno
			static int err;
			return &err;
		}
	}

} // namespace tmm

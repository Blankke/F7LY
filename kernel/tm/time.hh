//
// Copied from Li Shuang ( pseudonym ) on 2024-07-30
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#pragma once
#include "types.hh"
#include "hal/cpu.hh"
#include "proc/proc_manager.hh"
namespace tmm
{
	// following code is from Linux (include/uapi/linux/time.h)

	struct timespec
	{
		long tv_sec;  /* seconds */
		long tv_nsec; /* nanoseconds */
	};

	enum SystemClockId : uint
	{
		/*
		 * The IDs of the various system clocks (for POSIX.1b interval timers):
		 */
		CLOCK_REALTIME = 0,
		CLOCK_MONOTONIC = 1,
		CLOCK_PROCESS_CPUTIME_ID = 2,
		CLOCK_THREAD_CPUTIME_ID = 3,
		CLOCK_MONOTONIC_RAW = 4,
		CLOCK_REALTIME_COARSE = 5,
		CLOCK_MONOTONIC_COARSE = 6,
		CLOCK_BOOTTIME = 7,
		CLOCK_REALTIME_ALARM = 8,
		CLOCK_BOOTTIME_ALARM = 9,
		/*
		 * The driver implementing this got removed. The clock ID is kept as a
		 * place holder. Do not reuse!
		 */
		CLOCK_SGI_CYCLE = 10,
		CLOCK_TAI = 11,

		MAX_CLOCKS = 16
	};
	constexpr uint64 _1K_dec = 1000UL;
	constexpr uint64 _1M_dec = _1K_dec * _1K_dec;
	constexpr uint64 _1G_dec = _1M_dec * _1K_dec;
	// 以下两个常量表达式应当是绑定在一起的

	// 参考测例说明的时钟频率是12.5MHz,
	// 但是测试发现比这个值要小一些，大约是四分之一 constexpr uint64 qemu_fre =
	// 12 * _1M_dec + 500 * _1K_dec; constexpr uint64 qemu_fre = 100 * _1M_dec;
	// // 按照 CPUCFG.4 的值则恰为 100'000'000
	constexpr uint64 qemu_fre = 3 * _1M_dec + 125 * _1K_dec;
	// 由cycles计算出微秒的方法
	constexpr inline uint64 qemu_fre_cal_usec( uint64 cycles ) { return cycles * 8 / 25; }
	// 由微秒计算出cycles的方法
	constexpr inline  uint64 qemu_fre_cal_cycles( uint64 usec ) { return usec * 25 / 8; }

	// 100K 分频，则时间片 100K/3.125MHz ~ 32ms
	// constexpr uint div_fre = 0x80000000UL;
	constexpr uint div_fre	   = ( 200 * _1K_dec ) >> 2; // 低两位由硬件补齐
	constexpr uint ms_per_tick = div_fre * _1K_dec / qemu_fre;
	inline ulong get_main_frequence() { return qemu_fre; }

	inline ulong get_hw_time_stamp() { return ( (Cpu*)k_cpus[proc::k_pm.get_cur_cpuid()]. get_cpu() )->get_time(); }

	inline ulong time_stamp_to_usec( ulong ts ) { return qemu_fre_cal_usec( ts ); }

	inline ulong usec_to_time_stamp( ulong us ) { return qemu_fre_cal_cycles( us ); }

	inline ulong cycles_per_tick() { return div_fre << 2; }
	///@brief 如果这个头文件被多个 .cc 文件 #include，且未加 inline，每个 .cc 会生成函数代码 → 导致多个符号冲突。
} // namespace tmm

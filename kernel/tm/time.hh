//
// Copied from Li Shuang ( pseudonym ) on 2024-07-30
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

/**
 * @file time.hh
 * @brief 内核时间系统核心定义
 * 
 * 本文件定义了内核时间管理的基础数据结构和常量，包括：
 * - POSIX兼容的时间结构体
 * - 系统时钟类型枚举
 * - 硬件相关的时间转换函数
 * - 时钟频率和分频配置
 */

#pragma once
#include "types.hh"
#include "hal/cpu.hh"
#include "proc/proc_manager.hh"
#include <EASTL/atomic.h>

/// @brief POSIX定时器绝对时间标志
/// 用于timer_settime等函数，指示时间值为绝对时间而非相对时间
#define TIMER_ABSTIME 1

namespace tmm
{
	/**
	 * @brief POSIX标准时间结构体
	 * 用于表示高精度时间，精确到纳秒级别
	 * 遵循POSIX.1标准，兼容Linux内核时间接口
	 */
	struct timespec
	{
		long tv_sec;  ///< 秒数，自Unix纪元（1970-01-01 00:00:00 UTC）开始
		long tv_nsec; ///< 纳秒数，范围[0, 999999999]
	};

	/**
	 * @brief POSIX间隔定时器规格结构体
	 * 用于指定定时器的初始过期时间和重复间隔
	 * 遵循POSIX.1b标准，用于timer_settime()和timer_gettime()系统调用
	 */
	struct itimerspec
	{
		struct timespec it_interval; ///< 定时器间隔（0表示一次性定时器）
		struct timespec it_value;    ///< 定时器初始过期时间（0表示解除定时器）
	};

	/**
	 * @brief 系统时钟类型枚举
	 * 定义了POSIX.1b标准支持的各种系统时钟类型
	 * 不同的时钟类型有不同的语义和使用场景
	 */
	enum SystemClockId : uint
	{
		/// @brief 系统实时时钟，可被系统管理员调整
		/// 受系统时间设置影响，可能出现时间跳跃
		CLOCK_REALTIME = 0,
		
		/// @brief 单调时钟，从系统启动开始单调递增
		/// 不受系统时间调整影响，适用于测量时间间隔
		CLOCK_MONOTONIC = 1,
		
		/// @brief 进程CPU时间，测量调用进程消耗的CPU时间
		/// 包括用户态和内核态的执行时间
		CLOCK_PROCESS_CPUTIME_ID = 2,
		
		/// @brief 线程CPU时间，测量调用线程消耗的CPU时间
		/// 仅计算当前线程的执行时间
		CLOCK_THREAD_CPUTIME_ID = 3,
		
		/// @brief 原始单调时钟，不受NTP调整影响
		/// 提供更"原始"的单调时间，不经过频率调整
		CLOCK_MONOTONIC_RAW = 4,
		
		/// @brief 粗粒度实时时钟，性能更高但精度较低
		/// 适用于不需要高精度的应用场景
		CLOCK_REALTIME_COARSE = 5,
		
		/// @brief 粗粒度单调时钟，性能更高但精度较低
		/// 适用于不需要高精度的时间间隔测量
		CLOCK_MONOTONIC_COARSE = 6,
		
		/// @brief 系统启动时钟，包括系统挂起时间
		/// 类似MONOTONIC，但包括系统睡眠期间的时间
		CLOCK_BOOTTIME = 7,
		
		/// @brief 实时闹钟，可以在系统挂起时唤醒系统
		/// 用于需要在系统挂起时触发的定时器
		CLOCK_REALTIME_ALARM = 8,
		
		/// @brief 启动时间闹钟，基于BOOTTIME的闹钟
		/// 可以在系统挂起时基于启动时间触发
		CLOCK_BOOTTIME_ALARM = 9,
		
		/// @brief SGI周期计数器（已废弃）
		/// 原驱动已移除，此ID保留作为占位符，不应重用
		CLOCK_SGI_CYCLE = 10,
		
		/// @brief 国际原子时钟（TAI）
		/// 基于原子时标准，不包含闰秒调整
		CLOCK_TAI = 11,

		/// @brief 最大时钟数量限制
		MAX_CLOCKS = 16
	};
	
	/// @brief 时间单位常量定义
	/// @{
	constexpr uint64 _1K_dec = 1000UL;            ///< 1千（十进制）
	constexpr uint64 _1M_dec = _1K_dec * _1K_dec; ///< 1百万（十进制）
	constexpr uint64 _1G_dec = _1M_dec * _1K_dec; ///< 10亿（十进制）
	/// @}
	
	/**
	 * @brief 当前架构用于时间换算的硬件计时器频率。
	 *
	 * RISC-V 在 QEMU virt + OpenSBI 下暴露的 ACLINT timer 频率为 10MHz；
	 * LoongArch QEMU virt 的 constant timer/rdtime.d 频率按 100MHz 解释。
	 *
	 * 之前这里统一写死成 3.125MHz，会导致：
	 * 1. RISC-V 的 gettimeofday/clock_gettime 走时明显失真；
	 * 2. 基于 timeval 的短超时等待被严重放大或缩小；
	 * 3. 双架构的调度/超时行为缺乏可比性。
	 *
	 * LoongArch 之前沿用 3.125MHz 会把 rdtime.d 读数放大约 32 倍，iozone/lmbench
	 * 这类以 wall clock 反推吞吐的 benchmark 会被压低到 baseline 的一个数量级之外。
	 */
#ifdef RISCV
	constexpr uint64 qemu_fre = 10 * _1M_dec;
#elif defined(LOONGARCH)
	constexpr uint64 qemu_fre = 100 * _1M_dec;
#else
	constexpr uint64 qemu_fre = 10 * _1M_dec;
#endif
	
	/**
	 * @brief 硬件周期数转微秒
	 * @param cycles 硬件时钟周期数
	 * @return 对应的微秒数
	 *
	 * RISC-V 与 LoongArch 的 QEMU 计时器频率不同，换算必须统一使用
	 * qemu_fre。这里先拆成“整秒 + 余数”再相乘，避免长时间运行后
	 * cycles * 1000000 发生 64 位溢出。
	 */
	inline ulong get_main_frequence();

	inline uint64 qemu_fre_cal_usec( uint64 cycles ) {
		const uint64 frequency = get_main_frequence();
		return ( cycles / frequency ) * _1M_dec +
		       ( ( cycles % frequency ) * _1M_dec ) / frequency;
	}
	
	/**
	 * @brief 微秒转硬件周期数
	 * @param usec 微秒数
	 * @return 对应的硬件时钟周期数
	 */
	inline uint64 qemu_fre_cal_cycles( uint64 usec ) {
		const uint64 frequency = get_main_frequence();
		return ( usec / _1M_dec ) * frequency +
		       ( ( usec % _1M_dec ) * frequency ) / _1M_dec;
	}

	/**
	 * @brief 统一的内核调度 tick 周期（微秒）。
	 *
	 * 之前项目里 tick 大约在 64ms（LoongArch）到 195ms（RISC-V）之间，
	 * 对 `select/poll` 短超时、sleep 精度和单核多进程调度都过于粗糙。
	 * 这里统一收敛到 10ms，既能显著改善交互与并发响应，也不会把定时器中断频率拉得过高。
	 */
	constexpr uint64 tick_period_us = 10 * _1K_dec;

	/**
	 * @brief 硬件计数器视角下的每 tick 周期数。
	 *
	 * LoongArch 的定时器 CSR 低两位由硬件补齐，因此继续保留“右移再左移”的编码方式。
	 * RISC-V 侧虽然最终不会直接使用 div_fre 写寄存器，但仍通过 cycles_per_tick()
	 * 与其他共享代码保持同一套语义。
	 */
	/**
	 * @brief 每个tick对应的毫秒数
	 * 计算公式：(分频值 * 1000) / 时钟频率
	 */
	constexpr uint ms_per_tick = static_cast<uint>(tick_period_us / _1K_dec);
	
	/**
	 * @brief 获取主时钟频率
	 * @return 系统主时钟频率（Hz）
	 */
	inline ulong get_main_frequence() {
		static eastl::atomic<ulong> cached_frequency{0};
		const ulong cached = cached_frequency.load(eastl::memory_order_acquire);
		if (cached != 0)
		{
			return cached;
		}

		ulong detected = qemu_fre;
#if defined(LOONGARCH) && defined(BOARD_LS2K1000)
		// LoongArch CPUCFG4/5 描述恒定计数器的基频和倍率。物理板不能沿用
		// QEMU 的 100MHz 魔数，否则 sleep、futex 和所有 POSIX 时钟都会漂移。
		uint64 base = 0;
		uint64 ratio = 0;
		uint64 index = 4;
		asm volatile("cpucfg %0, %1" : "=r"(base) : "r"(index));
		index = 5;
		asm volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));
		const uint64 multiplier = ratio & 0xffffU;
		const uint64 divisor = (ratio >> 16) & 0xffffU;
		bool frequency_valid = false;
		if (base != 0 && multiplier != 0 && divisor != 0)
		{
			const uint64 quotient = base / divisor;
			const uint64 remainder = base % divisor;
			if (quotient <= ~0ULL / multiplier)
			{
				detected = static_cast<ulong>(quotient * multiplier +
				                              (remainder * multiplier) / divisor);
				frequency_valid = detected >= _1M_dec && detected <= 10ULL * _1G_dec;
			}
		}
		// 失真的恒定计数器频率会同时破坏中断、SATA 超时和 POSIX 时钟。
		// 实机上拒绝静默回退到 QEMU 频率，让固件/CPUCFG 问题可被直接定位。
		if (!frequency_valid)
		{
			panic("[time] invalid LS2K1000 constant timer frequency: base=%lu ratio=0x%lx",
			      base, ratio);
		}
#endif
		ulong expected = 0;
		cached_frequency.compare_exchange_strong(
			expected, detected, eastl::memory_order_acq_rel);
		return cached_frequency.load(eastl::memory_order_acquire);
	}

	inline uint64 cycles_to_units(uint64 cycles, uint64 units_per_second)
	{
		const uint64 frequency = get_main_frequence();
		return (cycles / frequency) * units_per_second +
		       ((cycles % frequency) * units_per_second) / frequency;
	}

	inline uint64 cycles_to_seconds(uint64 cycles) { return cycles_to_units(cycles, 1); }
	inline uint64 cycles_to_milliseconds(uint64 cycles) { return cycles_to_units(cycles, _1K_dec); }
	inline uint64 cycles_to_microseconds(uint64 cycles) { return cycles_to_units(cycles, _1M_dec); }
	inline uint64 cycles_to_nanoseconds(uint64 cycles) { return cycles_to_units(cycles, _1G_dec); }

	/**
	 * @brief 获取当前硬件时间戳
	 * @return 当前CPU的硬件时间戳（周期数）
	 * @note 从当前CPU的硬件计数器读取时间戳
	 */
	inline ulong get_hw_time_stamp() { 
		return ( (Cpu*)k_cpus[proc::k_pm.get_cur_cpuid()].get_cpu() )->get_time(); 
	}

	/**
	 * @brief 硬件时间戳转换为微秒
	 * @param ts 硬件时间戳（周期数）
	 * @return 对应的微秒数
	 */
	inline ulong time_stamp_to_usec( ulong ts ) { 
		return qemu_fre_cal_usec( ts ); 
	}

	/**
	 * @brief 微秒转换为硬件时间戳
	 * @param us 微秒数
	 * @return 对应的硬件时间戳（周期数）
	 */
	inline ulong usec_to_time_stamp( ulong us ) { 
		return qemu_fre_cal_cycles( us ); 
	}

	/**
	 * @brief 获取每个tick的硬件周期数
	 * @return 每个系统tick对应的硬件时钟周期数
	 * @note 低两位由硬件自动补齐，所以需要左移2位
	 */
	inline ulong cycles_per_tick() { 
		return (qemu_fre_cal_cycles(tick_period_us) >> 2) << 2;
	}

} // namespace tmm

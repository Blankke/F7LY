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
	 * @brief QEMU模拟器时钟频率配置
	 * 
	 * 根据实际测试和CPUCFG.4寄存器的值进行调整：
	 * - 理论频率：12.5MHz（参考测例说明）
	 * - 实际测试：约为理论值的1/4
	 * - CPUCFG.4值：100,000,000Hz
	 * - 当前使用：3.125MHz（3,125,000Hz）
	 */
	constexpr uint64 qemu_fre = 3 * _1M_dec + 125 * _1K_dec;
	
	/**
	 * @brief 硬件周期数转微秒
	 * @param cycles 硬件时钟周期数
	 * @return 对应的微秒数
	 * @note 转换公式：cycles * 1000000 / 3125000 = cycles * 8 / 25
	 */
	constexpr inline uint64 qemu_fre_cal_usec( uint64 cycles ) { 
		return cycles * 8 / 25; 
	}
	
	/**
	 * @brief 微秒转硬件周期数
	 * @param usec 微秒数
	 * @return 对应的硬件时钟周期数
	 * @note 转换公式：usec * 3125000 / 1000000 = usec * 25 / 8
	 */
	constexpr inline uint64 qemu_fre_cal_cycles( uint64 usec ) { 
		return usec * 25 / 8; 
	}

	/**
	 * @brief 定时器分频配置
	 * 
	 * 系统tick的生成配置：
	 * - 原始频率：3.125MHz
	 * - 分频值：200K >> 2 = 50K（低两位由硬件补齐为200K）
	 * - 实际分频：200K
	 * - tick频率：3.125MHz / 200K ≈ 15.625Hz
	 * - tick周期：1/15.625 ≈ 64ms
	 */
	constexpr uint div_fre = ( 200 * _1K_dec ) >> 2; ///< 分频值，低两位由硬件补齐
	
	/**
	 * @brief 每个tick对应的毫秒数
	 * 计算公式：(分频值 * 1000) / 时钟频率
	 */
	constexpr uint ms_per_tick = div_fre * _1K_dec / qemu_fre;
	
	/**
	 * @brief 获取主时钟频率
	 * @return 系统主时钟频率（Hz）
	 */
	inline ulong get_main_frequence() { 
		return qemu_fre; 
	}

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
		return div_fre << 2; 
	}

} // namespace tmm

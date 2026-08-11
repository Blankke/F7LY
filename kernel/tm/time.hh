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
#include "platform_clock_backend.hh"

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
	 * LoongArch 的定时器 CSR 低两位由硬件补齐，因此结果必须按 4 周期向上对齐。
	 * RISC-V 侧虽然最终不会直接使用 div_fre 写寄存器，但仍通过 cycles_per_tick()
	 * 与其他共享代码保持同一套语义。
	 */
	/**
	 * @brief 每个tick对应的毫秒数
	 * 计算公式：(分频值 * 1000) / 时钟频率
	 */
	constexpr uint ms_per_tick = static_cast<uint>(tick_period_us / _1K_dec);
	
	/**
	 * @brief 获取平台恒定计数器频率（Hz）。
	 *
	 * 平台差异由编译期选中的 clock backend 负责；通用时间代码不再识别
	 * 架构、开发板或 QEMU。
	 */
	inline uint64 clock_frequency_hz()
	{
		return platform::clock_backend::frequency_hz();
	}

	inline uint64 cycles_to_units(uint64 cycles, uint64 units_per_second)
	{
		const uint64 frequency = platform::clock_backend::frequency_hz();
		return (cycles / frequency) * units_per_second +
		       ((cycles % frequency) * units_per_second) / frequency;
	}

	inline uint64 cycles_to_seconds(uint64 cycles) { return cycles_to_units(cycles, 1); }
	inline uint64 cycles_to_milliseconds(uint64 cycles) { return cycles_to_units(cycles, _1K_dec); }
	inline uint64 cycles_to_microseconds(uint64 cycles) { return cycles_to_units(cycles, _1M_dec); }
	inline uint64 cycles_to_nanoseconds(uint64 cycles) { return cycles_to_units(cycles, _1G_dec); }

	/**
	 * @brief 微秒转换为硬件计数器周期数。
	 *
	 * 先拆成整秒和余数，避免长时间值直接相乘造成 64 位溢出。
	 */
	inline uint64 microseconds_to_cycles(uint64 usec)
	{
		const uint64 frequency = platform::clock_backend::frequency_hz();
		return (usec / _1M_dec) * frequency +
		       ((usec % _1M_dec) * frequency) / _1M_dec;
	}

	/**
	 * @brief 获取当前硬件时间戳
	 * @return 当前CPU的硬件时间戳（周期数）
	 * @note 从当前CPU的硬件计数器读取时间戳
	 */
	inline ulong get_hw_time_stamp() {
		return platform::clock_backend::read_ticks();
	}

	/**
	 * @brief 硬件时间戳转换为微秒
	 * @param ts 硬件时间戳（周期数）
	 * @return 对应的微秒数
	 */
	inline ulong time_stamp_to_usec( ulong ts ) { 
		return cycles_to_microseconds( ts );
	}

	/**
	 * @brief 微秒转换为硬件时间戳
	 * @param us 微秒数
	 * @return 对应的硬件时间戳（周期数）
	 */
	inline ulong usec_to_time_stamp( ulong us ) { 
		return microseconds_to_cycles( us );
	}

	/**
	 * @brief 获取每个tick的硬件周期数
	 * @return 每个系统tick对应的硬件时钟周期数
	 * @note 低两位由硬件自动补齐；向上对齐可保证一次 tick 不会提前触发
	 */
	inline ulong cycles_per_tick() { 
		return (microseconds_to_cycles(tick_period_us) + 3ULL) & ~3ULL;
	}

} // namespace tmm

//
// Copied from Li shuang ( pseudonym ) on 2024-04-05 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

/**
 * @file timer_manager.hh
 * @brief 内核定时器管理器头文件
 * 
 * 本文件定义了内核的时间管理功能，包括：
 * - 系统时间获取和管理
 * - 进程睡眠和唤醒机制
 * - 多种时钟类型支持（实时时钟、单调时钟、CPU时间等）
 * - POSIX兼容的时间接口
 */

#pragma once 

#include "types.hh"
#include "tm/time.hh"
#include "spinlock.hh"

namespace tmm
{
	// C接口函数声明，供C代码调用
	#ifdef __cplusplus
	extern "C"
	{
#endif

		/// @brief POSIX标准的clock_gettime函数声明
		/// @param clk 时钟ID类型
		/// @param tp 时间输出参数
		/// @return 成功返回0，失败返回负数
		int clock_gettime(clockid_t clk, struct timespec *tp);

		/// @brief 获取错误号存储位置的指针
		/// @return 指向错误号变量的指针
		int *__errno_location(void);

#ifdef __cplusplus
	}
#endif

	/**
	 * @brief 时间值结构体（Linux兼容）
	 * 用于存储秒和微秒精度的时间值，主要用于gettimeofday等系统调用
	 */
	struct timeval
	{
		using time_t = uint64;      ///< 时间类型定义（秒）
		using suseconds_t = uint64; ///< 亚秒时间类型定义（微秒）

		time_t      tv_sec;     ///< 秒数
		suseconds_t tv_usec;    ///< 微秒数 [0-999999]
	};

	/**
	 * @brief 进程时间统计结构体（Linux兼容）
	 * 用于times系统调用，记录进程和子进程的CPU使用时间
	 */
	struct tms
	{
		uint64 tms_utime;   ///< 当前进程用户态运行时间（时钟tick数）
		uint64 tms_stime;   ///< 当前进程内核态运行时间（时钟tick数）
		uint64 tms_cutime;  ///< 已结束子进程的用户态运行时间总和
		uint64 tms_cstime;  ///< 已结束子进程的内核态运行时间总和
	};

	/**
	 * @brief 分解时间结构体（ISO C标准）
	 * 将时间分解为年、月、日、时、分、秒等分量，便于格式化显示
	 */
	struct tm
	{
		int tm_sec;			///< 秒 [0-60] (允许1个闰秒)
		int tm_min;			///< 分 [0-59]
		int tm_hour;		///< 时 [0-23]
		int tm_mday;		///< 日 [1-31]
		int tm_mon;			///< 月 [0-11] (0表示1月)
		int tm_year;		///< 年份减去1900
		int tm_wday;		///< 星期几 [0-6] (0表示星期日)
		int tm_yday;		///< 年内天数 [0-365]
		int tm_isdst;		///< 夏令时标志 [-1/0/1]

		// 扩展字段（GNU兼容）
		# ifdef	__USE_MISC
		long int tm_gmtoff;		///< 相对UTC的秒数偏移
		const char *tm_zone;	///< 时区缩写字符串
		# else
		long int __tm_gmtoff;	///< 相对UTC的秒数偏移（内部使用）
		const char *__tm_zone;	///< 时区缩写字符串（内部使用）
		# endif
	};

	/**
	 * @brief 内核定时器管理器类
	 * 
	 * 负责管理系统的时间相关功能，包括：
	 * - 硬件时间戳管理
	 * - 系统tick计数
	 * - 进程睡眠调度
	 * - 多种时钟类型的时间获取
	 * - 时间格式转换
	 */
	class TimerManager
	{
		friend class trap_manager; ///< 允许trap管理器访问私有成员

	private:
		SpinLock _lock;  ///< 保护并发访问的自旋锁
		// uint64 _ticks;    // 废弃：tick计数已移至trap_mgr
		// uint64 _tcfg_data; // 废弃：定时器配置数据

	public:
		/// @brief 默认构造函数
		TimerManager() = default;

		/// @brief 初始化定时器管理器
		/// @param lock_name 锁的名称，用于调试
		void init( const char *lock_name );

		// 废弃的中断处理相关函数
		// int handle_clock_intr();
		// void tick_increase() { _lock.acquire(); _ticks++; _lock.release(); }
		// void open_ti_intr();
		// void close_ti_intr();

		/// @brief 获取当前系统时间（timeval格式）
		/// @return 包含秒和微秒的时间值
		/// @note 主要用于gettimeofday系统调用
		timeval get_time_val();

		/// @brief 使当前进程睡眠指定的tick数
		/// @param n 睡眠的tick数，必须非负
		/// @return 成功返回0，进程被杀死返回-2，参数错误返回-1
		int sleep_n_ticks( int n );

		/// @brief 根据timeval指定的时间进行睡眠
		/// @param tv 睡眠时间（秒+微秒）
		/// @return 成功返回0，失败返回负数
		int sleep_from_tv( timeval tv );

		/// @brief 获取指定时钟的当前时间
		/// @param clockid 时钟类型ID
		/// @param tp 输出参数，存储时间值
		/// @return 成功返回0，失败返回负数
		/// @note 支持多种POSIX时钟类型
		int clock_gettime( SystemClockId clockid, timespec * tp );

		/// @brief 获取当前系统tick计数
		/// @return 从系统启动以来的tick数
		/// @note tick是系统时间的基本单位
		uint64 get_ticks() ;
	};

	/// @brief 全局定时器管理器实例
	/// @note 整个内核共享一个定时器管理器实例
	extern TimerManager k_tm;

} // namespace tmm

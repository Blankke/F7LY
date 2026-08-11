//
// Copied from Li Shuang ( pseudonym ) on 2024-07-12 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

/**
 * @file timer_interface.cc
 * @brief 定时器模块接口函数实现
 * 
 * 本文件实现了定时器模块对外提供的接口函数，
 * 作为其他内核模块访问定时器功能的入口点。
 * 这些函数封装了TimerManager的功能，提供简洁的C风格接口。
 */

#include <timer_interface.hh>
#include "tm/timer_manager.hh"

namespace tmm
{
	/**
	 * @brief 获取当前系统tick计数
	 * @return 自系统启动以来的tick数
	 * 
	 * 该函数提供对系统tick计数的访问，tick是系统时间管理的基础单位。
	 * 通过委托给全局TimerManager实例来获取tick计数。
	 * 
	 * 使用场景：
	 * - 进程调度时间片计算
	 * - 睡眠超时检查
	 * - 系统运行时间统计
	 */
	ulong get_ticks()
	{
		return tmm::k_tm.get_ticks();
	}

	void *get_tick_wait_channel()
	{
		return tmm::k_tm.get_tick_wait_channel();
	}

} // namespace tmm

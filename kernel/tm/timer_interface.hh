//
// Copied from Li Shuang ( pseudonym ) on 2024-07-12 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

/**
 * @file timer_interface.hh
 * @brief 定时器模块对外接口声明
 * 
 * 本文件声明了定时器模块提供给其他内核模块使用的公共接口函数，
 * 这里只保留有独立翻译单元实现的 tick 访问接口。时钟频率、周期换算等
 * 内联定义统一来自 time.hh，不能再维护一组只有声明的影子接口。
 */

#pragma once
#include "time.hh"

namespace tmm
{
	/**
	 * @brief 获取当前系统tick计数
	 * @return 自系统启动以来的tick数
	 */
	extern ulong get_ticks();

	/**
	 * @brief 获取“每个 tick 都会被唤醒”的内核等待通道
	 * @return 可直接作为 sleep/wakeup 键使用的稳定地址
	 */
	extern void *get_tick_wait_channel();

} // namespace tmm

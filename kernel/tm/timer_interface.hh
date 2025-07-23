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
 * 包括tick中断处理、时间戳获取、时间转换等核心功能。
 * 这些函数是内核时间管理的基础设施。
 */

#pragma once
#include "printer.hh"

namespace tmm
{
	/**
	 * @brief 处理系统tick中断
	 * @return 中断处理结果状态码
	 * 
	 * 该函数由中断处理程序调用，负责：
	 * - 更新系统tick计数
	 * - 唤醒睡眠超时的进程
	 * - 执行定时任务调度
	 * - 更新进程时间统计
	 */
	extern int handle_tick_intr();

	/**
	 * @brief 获取当前系统tick计数
	 * @return 自系统启动以来的tick数
	 */
	extern ulong get_ticks();

	/**
	 * @brief 获取系统主时钟频率
	 * @return 主时钟频率（Hz）
	 */
	extern ulong get_main_frequence();

	/**
	 * @brief 获取每个tick的硬件周期数
	 * @return 每个tick对应的硬件时钟周期数
	 */
	extern ulong cycles_per_tick();

	/**
	 * @brief 获取当前硬件时间戳
	 * @return 当前的硬件时钟周期计数
	 */
	extern ulong get_hw_time_stamp();

	/**
	 * @brief 硬件时间戳转换为微秒
	 * @param ts 硬件时间戳（周期数）
	 * @return 对应的微秒数
	 * 
	 * 将底层硬件计数器值转换为标准的微秒时间单位，
	 * 便于与POSIX时间接口兼容。
	 */
	extern ulong time_stamp_to_usec( ulong ts );

	/**
	 * @brief 微秒转换为硬件时间戳
	 * @param us 微秒数
	 * @return 对应的硬件时间戳（周期数）
	 * 
	 * 将标准微秒时间单位转换为硬件计数器值，
	 * 用于设置硬件定时器和计算时间差。
	 */
	extern ulong usec_to_time_stamp( ulong us );

} // namespace tmm

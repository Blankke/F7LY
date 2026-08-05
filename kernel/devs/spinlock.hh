

#pragma once

#include "../types.hh"

#include <EASTL/atomic.h>


class Cpu;

class SpinLock
{
private:
	const char *_name = nullptr;
	eastl::atomic<Cpu *> _locked;
public:
	SpinLock();

	/// @brief init spinlock
	/// @param name for debugging
	void init(const char *name);

	/// @brief request for spinlock
	void acquire();

	/// @brief 尝试获取锁；失败时不会自旋，供 SMP 调度器跳过正在其它 CPU 上运行的任务。
	bool try_acquire();

	/**
	 * @brief 原子完成“当前 CPU 已持有则复用，否则阻塞获取”。
	 * @return 本次真正获取锁返回 true；调用前已由当前 CPU 持有返回 false。
	 *
	 * 不能在调用方用 is_held() 与 acquire() 拼接这一语义：两次调用之间
	 * 仍可能被时钟中断抢占，恢复后会把同一 CPU 已经持有的锁再次获取。
	 */
	bool acquire_unless_held();

	/**
	 * @brief 原子尝试获取；锁已由当前 CPU 持有或正被其它 CPU 持有时返回 false。
	 * @return 仅当本次成功获取锁时返回 true。
	 */
	bool try_acquire_unless_held();

	/// @brief release spinlock
	void release();

	bool is_held();

	/// @brief get name of spinlock
	/// @return name of spinlock
	const char *get_name() const { return _name; }

};

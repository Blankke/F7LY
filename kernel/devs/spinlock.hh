

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

	/// @brief release spinlock
	void release();

	bool is_held();

	/// @brief get name of spinlock
	/// @return name of spinlock
	const char *get_name() const { return _name; }

};

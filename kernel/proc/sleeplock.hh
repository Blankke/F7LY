//
// Copy from Li shuang ( pseudonym ) on 2024-04-23 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 

#include "types.hh"

#include "spinlock.hh"

namespace proc
{
	class SleepLock
	{
	private:
		bool _locked = false;
		SpinLock _lock;
		// 线程组内多个 PCB 共享 pid；睡眠锁所有者必须按 tid 区分。
		uint _owner_tid;
		// for debugging 
		const char *_name;
	public:
		SleepLock() {};
		void init( const char *lock_name, const char *name );
		void acquire();
		void release();
		bool is_holding();
	};
} // namespace proc

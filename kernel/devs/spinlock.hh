

#pragma once

#include "../types.hh"

#include <EASTL/atomic.h>

class SpinLock
{
private:
	const char *_name = nullptr;
	int _locked = 0;

public:
	SpinLock();

	/// @brief init spinlock
	/// @param name for debugging
	void init(const char *name);

	/// @brief request for spinlock
	void acquire();

	/// @brief release spinlock
	void release();

	bool is_held()
	{
		return _locked;
	};
};

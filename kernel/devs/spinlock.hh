

#pragma once

#include "../types.hh"

#include <EASTL/atomic.h>


class Cpu;

class SpinLock
{
private:
	const char *_name = nullptr;
	eastl::atomic<Cpu *> _locked ;
public:
	SpinLock();

	/// @brief init spinlock
	/// @param name for debugging
	void init(const char *name);

	/// @brief request for spinlock
	void acquire();

	/// @brief release spinlock
	void release();

	bool is_held();

};

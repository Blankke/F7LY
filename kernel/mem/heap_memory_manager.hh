#pragma once
#include "spinlock.hh"
#include "buddysystem.hh"
#include "liballoc_allocator.hh"

namespace mem
{
 	class HeapMemoryManager
	{
	private:
		SpinLock _lock;

		BuddySystem* _k_allocator_coarse;

		L_Allocator _k_allocator_fine;

	public:
		HeapMemoryManager() {};
		void init( const char *lock_name );

		void *allocate( uint64 size );

		void free( void *p );
	};

    extern HeapMemoryManager k_hmm;
} // namespace mem

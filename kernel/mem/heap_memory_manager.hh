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
			void init( const char *lock_name ,uint64_t heap_start, uint64_t heap_size);

			void *allocate( uint64 size );

			void free( void *p );
			void get_stats(uint64 &cache_size, uint64 &used_size, uint32 &chunk_count, uint64 &coarse_free_pages, uint32 &coarse_max_block_pages);
		};

    extern HeapMemoryManager k_hmm;
} // namespace mem

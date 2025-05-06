
#include "heap_memory_manager.hh"
#include "pagetable.hh"
#include "memlayout.hh"
#include "klib.hh"

namespace mem
{
	HeapMemoryManager k_hmm;

	void HeapMemoryManager::init( const char *lock_name )
	{
		_lock.init( lock_name );

		// _k_allocator_coarse= (BuddySystem*)vm_kernel_heap_start;
		// _k_allocator_coarse->Initialize();
        uint64_t heap_start = (uint64_t)vm_kernel_heap_start;
        heap_start = (heap_start + PGSIZE - 1) & ~(PGSIZE - 1); //将pa_start向高地址对齐到PGSIZE的整数倍
        //仿照pmm的初始化方式，将buddy系统初始化到heap_start处
        _k_allocator_coarse = reinterpret_cast<BuddySystem*>(heap_start);
        heap_start += BSSIZE * PGSIZE;
        memset(_k_allocator_coarse, 0, BSSIZE * PGSIZE);
        _k_allocator_coarse->Initialize(heap_start);



		_k_allocator_fine.init(
			"kernel heap allocator - liballoc",
			_k_allocator_coarse
		);
	}

	void * HeapMemoryManager::allocate( uint64 size )
	{
		// if ( size < pg_size )
		// {
		// 	log_warn(
		// 		"dynamic memory allocation not implement.\n"
		// 		"too small memory allocating will not success.\n"
		// 		">> request size = %d bytes", size
		// 	);
		// 	return nullptr;
		// }
		// log_trace( "申请内存分配 %d bytes", size );
		// return _k_allocator_coarse.allocate_pages( ( size + pg_size - 1 ) / pg_size );
		return _k_allocator_fine.malloc( size );
	}

	void HeapMemoryManager::free( void *p )
	{
		// log_trace( "内存释放" );
		_k_allocator_fine.free( p );
	}
} // namespace mm

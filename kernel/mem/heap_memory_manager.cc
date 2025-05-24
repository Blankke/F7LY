
#include "heap_memory_manager.hh"
#include "pagetable.hh"
#include "memlayout.hh"
#include "klib.hh"
#include "printer.hh"
namespace mem
{
	HeapMemoryManager k_hmm;

	void HeapMemoryManager::init( const char *lock_name ,uint64_t heap_start)
	{
		_lock.init( lock_name );

		// _k_allocator_coarse= (BuddySystem*)vm_kernel_heap_start;
		// _k_allocator_coarse->Initialize();
		//由于用户态可能会使用堆内存，所以需要把堆内存的起始地址传进来。
        heap_start = (heap_start + PGSIZE - 1) & ~(PGSIZE - 1); //将pa_start向高地址对齐到PGSIZE的整数倍
        //仿照pmm的初始化方式，将buddy系统初始化到heap_start处
        _k_allocator_coarse = reinterpret_cast<BuddySystem*>(heap_start);
        heap_start += BSSIZE * PGSIZE;
        memset(_k_allocator_coarse, 0, BSSIZE * PGSIZE);

        _k_allocator_coarse->Initialize(heap_start);
		/*在原本的hmm中初始化时，粗粒度的buddy是紧耦合在hmm上的，
		它的初始化会把堆区域的内存全部初始化（也就是虚拟地址映射到物理地址上），
		但是这里我们不需要这样做，我们需要把堆内存初始化的时间改到vmm中，这里就不需要初始化*/


		_k_allocator_fine.init(
			"kernel heap allocator - liballoc",
			_k_allocator_coarse
		);
		//这里细粒度的管理是依仗着粗粒度进行的，它每一次申请内存的时候都会调用粗粒度的buddy系统，分配一个页面
		//再从这样分配的页面中，进行更细粒度的内存分配。
		printfGreen("HeapMemoryManager init\n");
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

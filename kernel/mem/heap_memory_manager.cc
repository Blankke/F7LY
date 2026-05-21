
#include "heap_memory_manager.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined (LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "memlayout.hh"
#include "klib.hh"
#include "printer.hh"
#include "physical_memory_manager.hh"

namespace mem
{
	HeapMemoryManager k_hmm;

	void HeapMemoryManager::init( const char *lock_name ,uint64_t heap_start, uint64_t heap_size)
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

        uint64 heap_pages = heap_size / PGSIZE;
        if (heap_pages == 0)
        {
            panic("[hmm] heap size too small");
        }
        _k_allocator_coarse->Initialize(heap_start, heap_pages);
		/*在原本的hmm中初始化时，粗粒度的buddy是紧耦合在hmm上的，
		它的初始化会把堆区域的内存全部初始化（也就是虚拟地址映射到物理地址上），
		但是这里我们不需要这样做，我们需要把堆内存初始化的时间改到vmm中，这里就不需要初始化*/


		_k_allocator_fine.init(
			"kernel heap allocator - liballoc",
			_k_allocator_coarse
		);
		//这里细粒度的管理是依仗着粗粒度进行的，它每一次申请内存的时候都会调用粗粒度的buddy系统，分配一个页面
		//再从这样分配的页面中，进行更细粒度的内存分配。
		printfGreen("[hmm] Heap Memory Manager Init at %p\n", this);
	}

		void * HeapMemoryManager::allocate( uint64 size )
		{
		// 全局 new/delete 需要服务普通 C++ 对象和 EASTL 容器，
		// 这里必须使用细粒度分配器，不能再把每个对象都当成整页来分配/释放。
		// 否则一旦释放路径遇到非页对齐对象，就会在 kfree! 处直接崩掉。
		if (size == 0)
		{
			size = 1;
		}

			void *ptr = _k_allocator_fine.malloc(size);
			if (ptr == nullptr)
			{
				uint64 cache_size = 0;
				uint64 used_size = 0;
				uint32 chunk_count = 0;
				uint64 coarse_free_pages = 0;
				uint32 coarse_max_block_pages = 0;
				get_stats(cache_size, used_size, chunk_count, coarse_free_pages, coarse_max_block_pages);
				panic("[hmm] alloc failed, size=%p, heap_total=%p, heap_used=%p, heap_cached=%p, chunks=%d, coarse_free_pages=%p, coarse_max_block_bytes=%p",
				      (void *)size,
				      (void *)mem::k_pmm.get_heap_allocator_size(),
				      (void *)used_size,
				      (void *)cache_size,
				      chunk_count,
				      (void *)coarse_free_pages,
				      (void *)(static_cast<uint64>(coarse_max_block_pages) * PGSIZE));
			}
			return ptr;
		}

	void HeapMemoryManager::free( void *p )
	{
		if (p == nullptr)
		{
			return;
		}

			// 与 allocate() 配对，统一交给细粒度分配器回收。
			_k_allocator_fine.free(p);
		}

		void HeapMemoryManager::get_stats(uint64 &cache_size, uint64 &used_size, uint32 &chunk_count, uint64 &coarse_free_pages, uint32 &coarse_max_block_pages)
		{
			_k_allocator_fine.get_stats(cache_size, used_size, chunk_count);
			coarse_free_pages = _k_allocator_coarse->get_free_page_count();
			coarse_max_block_pages = _k_allocator_coarse->get_max_free_block_pages();
		}
	} // namespace mem

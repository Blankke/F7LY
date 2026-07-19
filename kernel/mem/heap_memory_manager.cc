
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

        uint64 heap_pages = heap_size / PGSIZE;
        if (heap_pages == 0)
        {
            panic("[hmm] heap size too small");
        }

        // heap_start 指向 PMM 预留的 heap metadata 起点，heap_size 只表示
        // 实际可分配的数据区大小。二者的布局算法与 PMM 使用同一套
        // required_storage_bytes()，不会再浪费固定 320 页，也不会随容量溢出。
        heap_start = PGROUNDUP(heap_start);
        const uint64 tree_bytes = BuddySystem::required_tree_bytes(heap_pages);
        const uint64 metadata_bytes = PGROUNDUP(BuddySystem::required_storage_bytes(heap_pages));
        _k_allocator_coarse = reinterpret_cast<BuddySystem *>(heap_start);
        void *tree_storage = reinterpret_cast<void *>(heap_start + sizeof(BuddySystem));
        uint64 managed_start = heap_start + metadata_bytes;
        memset(reinterpret_cast<void *>(heap_start), 0, metadata_bytes);
        _k_allocator_coarse->Initialize(managed_start, heap_pages, tree_storage, tree_bytes);
		/*在原本的hmm中初始化时，粗粒度的buddy是紧耦合在hmm上的，
		它的初始化会把堆区域的内存全部初始化（也就是虚拟地址映射到物理地址上），
		但是这里我们不需要这样做，我们需要把堆内存初始化的时间改到vmm中，这里就不需要初始化*/


		_k_allocator_fine.init(
			"kernel heap allocator - liballoc",
			_k_allocator_coarse
		);
		//这里细粒度的管理是依仗着粗粒度进行的，它每一次申请内存的时候都会调用粗粒度的buddy系统，分配一个页面
		//再从这样分配的页面中，进行更细粒度的内存分配。
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

	void *HeapMemoryManager::try_allocate(uint64 size)
	{
		if (size == 0)
		{
			size = 1;
		}
		return _k_allocator_fine.malloc(size);
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

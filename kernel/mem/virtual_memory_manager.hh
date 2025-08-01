#pragma once 

#include "spinlock.hh"
#include <EASTL/string.h>
// 根据不同架构包含不同的页表实现
#ifdef RISCV
#include "riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "loongarch/pagetable.hh"

#endif

// 前向声明
namespace proc {
    struct vma;
}

namespace mem
{
	class VirtualMemoryManager
	{
	private:
		SpinLock _virt_mem_lock;

	public:
		static uint64 kstack_vm_from_global_id( uint global_id );

	public:
		VirtualMemoryManager() {};
		void init( const char *lock_name );
		/// @brief map va to pa through pt 
		/// @param pt pagetable to use 
		/// @param va virtual address 
		/// @param size mappint size 
		/// @param pa physical address 
		/// @param flags page table entry flags 
		/// @return success if true 
		bool map_pages( PageTable &pt, uint64 va, uint64 size, uint64 pa, uint64 flags );
		PageTable kvmmake();//创建内核页表，在初始化的时候调用
		void kvmmap(PageTable &pt, uint64 va, uint64 pa, uint64 sz, uint64 perms);//映射内核页表


		uint64 vmalloc(PageTable &pt, uint64 old_sz, uint64 new_sz, uint64 flags);

		uint64 vmdealloc( PageTable &pt, uint64 old_sz, uint64 new_sz );

		/// @brief unmap va from pt
		/// @param pt pagetable to use
		/// @param va virtual address
		/// @param npages num of pages to unmap
		/// @param do_free free physical pages?
		void vmunmap( PageTable &pt, uint64 va, uint64 npages, int do_free );

		PageTable vm_create();

		int vm_copy( PageTable &old_pt, PageTable &new_pt, uint64 start, uint64 size );

		/// @brief allocate shm
		/// @param pt pagetable to use
		/// @param oldshm oldshm lower address
		/// @param newshm newshm lower address
		/// @param sz shmsize
		/// @param phyaddr 
		/// @return newshm if success
		// uint64 allocshm( PageTable &pt, uint64 oldshm, uint64 newshm, uint64 sz, void *phyaddr[ pm::MAX_SHM_PGNUM ] );
		

		int copy_in( PageTable &pt, void *dst, uint64 src_va, uint64 len );

		int copy_str_in( PageTable &pt, void *dst, uint64 src_va, uint64 max );
		int copy_str_in( PageTable &pt, eastl::string &dst, uint64 src_va, uint64 max );



		/// @brief map shm pages to physical pages, it is similar with map_pages
		/// @param pt pagetable to use
		/// @param oldshm oldshm lower address
		/// @param newshm newshm lower address
		/// @param sz shmsize
		/// @param phyaddr 
		/// @return newshm if success
		// uint64 mapshm( PageTable &pt, uint64 oldshm, uint64 newshm, uint sz, void **phyaddr );

		/// @brief deallocate shm , when allocate shm failed
		/// @param pt pagetable to use
		/// @param oldshm oldshm lower address
		/// @param newshm newshm lower address 
		/// @return oldshm if success
		// uint64 deallocshm(PageTable &pt, uint64 oldshm, uint64 newshm );

		/// @brief copy from kernel to user
		/// @param pt pagetable to use
		/// @param va dest virtual address 
		/// @param p source address
		/// @param len length
		/// @return 0 if success, -1 if failed
		int copy_out( PageTable &pt, uint64 va, const void *p, uint64 len );

		/// @brief 为VMA惰性分配页面，统一处理mmap的各种标志和权限
		/// @param pt 页表
		/// @param va 虚拟地址
		/// @param vm VMA结构指针
		/// @param access_type 访问类型：0=读取, 1=写入, 2=执行
		/// @return 成功返回0，失败返回-1
		int allocate_vma_page(PageTable &pt, uint64 va, struct proc::vma *vm, int access_type);

		/// @brief mark a PTE invalid for user access
		/// @param pt 
		/// @param va 
		void uvmclear( PageTable &pt, uint64 va );

		/// @brief allocate memory to grow process from oldsz to newsz
		/// @param pt pagetable to use 
		/// @param oldsz old size
		/// @param newsz new size
		/// @return
		uint64 uvmalloc(PageTable &pt, uint64 oldsz, uint64 newsz, uint64 flags);

		/// @brief deallocate memory to shrink process from oldsz to newsz
		/// @param pt pagetable to use
		/// @param oldsz old size
		/// @param newsz new size
		/// @return 
		uint64 uvmdealloc( PageTable &pt, uint64 oldsz, uint64 newsz );

		/// @brief Initialize first user process virtual memory
		/// @param pt pagetable to use
		/// @param src source address of program code
		/// @param sz size of program code
		/// @return total allocated virtual memory size
		uint64 uvmfirst(PageTable &pt, uint64 src, uint64 sz);

		int protectpages(PageTable &pt, uint64 va, uint64 size, int perm, bool is_vma = false);

	private:
	};

	extern VirtualMemoryManager k_vmm;
}
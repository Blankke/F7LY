#pragma once 

#include "spinlock.hh"
#include "proc/vm_area.hh"
#include <EASTL/string.h>
#include <EASTL/vector.h>
// 根据不同架构包含不同的页表实现
#ifdef RISCV
#include "riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "loongarch/pagetable.hh"

#endif

namespace proc
{
	class ProcessMemoryManager;
}

namespace mem
{
#ifdef RISCV
	// Sv39 的 RSW[0] 由硬件忽略，统一作为软件 COW 标志使用。
	inline constexpr uint64 k_riscv_pte_cow = 1UL << 8;
#endif

	enum class UnmapTlbMode : uint8
	{
		Invalidate,
		// 仅允许 ProcessMemoryManager 在最后引用归零、活跃 CPU 掩码为空后使用。
		// 旧 ASID 必须保持退休状态，直到下一次全核 TLB 屏障后才可复用。
		SkipInactiveFinalTeardown,
	};

	class VirtualMemoryManager
	{
	private:
		SpinLock _virt_mem_lock;

	public:
		static uint64 kstack_vm_from_global_id( uint global_id );

	public:
		VirtualMemoryManager() {};
		void init( const char *lock_name );
		// 内核页表由引导核一次性建立，但地址翻译寄存器属于每个 CPU。
		// 次核放行后必须调用此函数，才能安全访问内核高地址栈、用户陷阱页和
		// 其它通过内核页表映射的全局对象。
		void activate_kernel_pagetable();
		// 页表层级的创建和保留高地址映射会修改共享页表。SMP 下 CLONE_VM
		// 线程可同时从 trap 返回，调用方需用这把自旋锁串行化“检查后建表”的
		// 临界区，避免两个 CPU 各自分配下级页表后互相覆盖父级 PTE。
		void lock_page_table_updates();
		void unlock_page_table_updates();
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
		void vmunmap(PageTable &pt,
		              uint64 va,
		              uint64 npages,
		              int do_free,
		              UnmapTlbMode tlb_mode = UnmapTlbMode::Invalidate);

		/**
		 * @brief 撤销最多 256 个不连续虚拟页，并把 TLB/PMM 操作合并成一批。
		 *
		 * addresses 可以无序且包含尚未驻留的页；有效叶子 PTE 会先全部清除，
		 * 随后只做一次保守范围失效，再在一次 PMM 锁内归还物理页。
		 */
		void vmunmap_sparse(PageTable &pt,
		                    const uint64 *addresses,
		                    uint32 count,
		                    int do_free,
		                    UnmapTlbMode tlb_mode = UnmapTlbMode::Invalidate);

		PageTable vm_create();

		/**
		 * @brief 把已驻留用户页复制到新页表，并把可写私有页降级为 COW。
		 *
		 * defer_parent_tlb_flush 只允许地址空间唯一持有者在 fork 批处理中使用：
		 * 调用方必须在返回用户态前完成一次全核 TLB 失效。返回 1 表示父页表
		 * 确实发生了 COW 权限降级，0 表示没有变化，失败返回 -1。
		 */
		int vm_copy(PageTable &old_pt,
		            PageTable &new_pt,
		            uint64 start,
		            uint64 size,
		            bool defer_parent_tlb_flush = false,
		            eastl::vector<proc::CowRollbackRange> *rollback_ranges = nullptr);

		/// @brief allocate shm
		/// @param pt pagetable to use
		/// @param oldshm oldshm lower address
		/// @param newshm newshm lower address
		/// @param sz shmsize
		/// @param phyaddr 
		/// @return newshm if success
		// uint64 allocshm( PageTable &pt, uint64 oldshm, uint64 newshm, uint64 sz, void *phyaddr[ pm::MAX_SHM_PGNUM ] );
		
		// target_mm 表示 pt 所属的用户地址空间；copy 期间需要用它处理懒分配、栈增长和 COW。
		// 传空时只允许从当前运行进程且页表基址一致的场景推导。
		int copy_in( PageTable &pt, void *dst, uint64 src_va, uint64 len, proc::ProcessMemoryManager *target_mm = nullptr );
		// 只确认用户读范围可访问，并按需补齐合法 VMA 页面；不搬运数据。
		int ensure_user_read_range( PageTable &pt, uint64 src_va, uint64 len );
		// 只确认用户写范围可访问，必要时完成懒分配/COW；不改写用户数据。
		int ensure_user_write_range( PageTable &pt, uint64 dst_va, uint64 len );
		// 解析单个用户读地址到内核可访问地址，用于同页小块拷贝快路径。
		int user_read_kernel_address( PageTable &pt, uint64 src_va, uint64 &kernel_addr );

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
		// 写用户页可能触发目标地址空间的缺页或 COW 拆页，target_mm 必须和 pt 指向同一个 mm。
		int copy_out( PageTable &pt, uint64 va, const void *p, uint64 len, proc::ProcessMemoryManager *target_mm = nullptr );

		/// @brief 处理 fork COW 写时复制页。
		/// @return 成功拆页/恢复写权限返回0；不是 COW 页或失败返回-1。
		int resolve_cow_page(PageTable &pt,
		                     uint64 va,
		                     proc::ProcessMemoryManager *target_mm = nullptr);

		/// @brief 为VMA惰性分配页面，统一处理mmap的各种标志和权限
		/// @param pt 页表
		/// @param va 虚拟地址
		/// @param vm VMA结构指针
		/// @param access_type 访问类型：0=读取, 1=写入, 2=执行
		/// @return 成功返回0，失败返回-1
		int allocate_vma_page(PageTable &pt, uint64 va, proc::vma *vm, int access_type);

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

		/**
		 * @brief 批量更新页表权限。
		 *
		 * pte_changed 用于告诉调用方本次是否真的写过叶子 PTE。VMA 的
		 * 权限元数据即使发生变化，尚未驻留的懒分配页也不会产生 TLB
		 * 翻译，因此不能为每个 mprotect 无条件执行一次 sfence。
		 */
		int protectpages(PageTable &pt,
		                 uint64 va,
		                 uint64 size,
		                 int prot,
		                 bool is_vma = false,
		                 bool *pte_changed = nullptr);

	private:
	};

	extern VirtualMemoryManager k_vmm;
}

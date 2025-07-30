#pragma once

#include "types.hh"
#include "pte.hh"
#include "printer.hh"
namespace mem
{

	extern bool debug_trace_walk;
	class PageTable
	{
	private:
		uint64 _base_addr;
        int* _ref = nullptr; // 引用计数指针，用于支持clone CLONE_VM标志位
		bool _is_global = false;

	public:
		PageTable() {};
		PageTable(uint64 addr) { _base_addr = addr; };
		~PageTable() { /* 注意：不在析构函数中调用dec_ref，而是显式调用 */ }
		void set_base(uint64 addr) { _base_addr = addr; }
		uint64 get_base() const { return _base_addr; }
		void set_global() { _is_global = true; }
		void unset_global() { _is_global = false; }
		bool is_null() { return _base_addr == 0; }

		// 引用计数管理
		void init_ref(); // 初始化引用计数
		void inc_ref(); // 增加引用计数
		void dec_ref(); // 减少引用计数
		int get_ref_count(); // 获取引用计数
		void share_from(const PageTable& other); // 从另一个页表共享（浅拷贝）

		/// @brief 软件遍历页表，通常，只能由全局页目录调用
		/// @param va virtual address
		/// @param alloc either alloc physical page or not
		/// @return PTE in the last level page table
		Pte walk(uint64 va, bool alloc);

		/// @brief 软件遍历页表，通常只能由用户的全局页目录调用
		/// @param va virtual address
		/// @return physical address mapped from va
		void *walk_addr(uint64 va);

		/// @brief 递归地释放页表中的所有页面
		void freewalk();

		/// @brief 递归地释放页表及其映射的所有页
		void freewalk_mapped();

		Pte kwalkaddr(uint64 va);
		ulong kwalk_addr( uint64 va );
		uint64 dir_num(int level, uint64 va);

		uint64 get_pte_data(uint64 index) { return (uint64)((pte_t *)_base_addr)[index]; }
		void reset_pte_data(uint64 index) { ((pte_t *)_base_addr)[index] = 0; }
		uint64 get_pte_addr(uint64 index) { return (uint64) & ((pte_t *)_base_addr)[index]; }
		Pte get_pte(uint64 index) { return Pte(&((pte_t *)_base_addr)[index]); }
		void set_pte(uint64 index, Pte pte) { ((pte_t *)_base_addr)[index] = pte.get_data(); }
		// 新增的函数，返回 Pte 的引用
		Pte &get_pte_ref(uint64 index)
		{
			// printfCyan("_base_addr)[index]: %d\n", (uint64 *) (_base_addr)[index]);
			printfCyan("_base_addr)[index]: %p\n", ((pte_t *)_base_addr)[index] );
			printfBlue("index: %d\n", index);
			printfBlue("current_pt: %p\n", _base_addr);
			return *(Pte *)&(((pte_t *)_base_addr)[index]);
		}

		void print_page_table();
		void print_all_map();

	private:
		bool _walk_to_next_level(Pte pte, bool alloc, PageTable &pt);
		void _vmprint(int level, uint64 va_base);
	};

	extern PageTable k_pagetable;
}

	constexpr bool is_page_align( uint64 addr )
	{
		ulong pg_sz =PGSIZE;
		return ( addr & ( pg_sz - 1 ) ) == 0;
	}
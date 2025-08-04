//
// Copy from Li shuang ( pseudonym ) on 2024-04-02
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//
#ifdef LOONGARCH
#include "physical_memory_manager.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "pte.hh"
#include "memlayout.hh"
#include "platform.hh"
#include "common.hh"
#include "page.hh"
namespace mem
{
	PageTable k_pagetable;

	bool debug_trace_walk = false;

	// 阶段1注释：移除分散的页表引用计数管理，统一使用ProcessMemoryManager
	/*
	// 引用计数管理实现
	void PageTable::init_ref() {
		if (_ref == nullptr && _base_addr != 0) {
			_ref = new int(1); // 初始引用计数为1
			printfCyan("init_ref: initialized page table %p with ref count: 1\n", _base_addr);
		} else if (_ref != nullptr) {
			panic("init_ref: page table %p already has ref count: %d\n", _base_addr, *_ref);
		} else {
			panic("init_ref: warning - page table has null base address\n");
		}
	}

	void PageTable::inc_ref() {
		if (_ref != nullptr) {
			(*_ref)++;
			printfCyan("inc_ref: page table %p, new ref count: %d\n", _base_addr, *_ref);
		} else {
			panic("inc_ref: warning - trying to increase ref count on page table %p with null ref pointer\n", _base_addr);
		}
	}

	void PageTable::dec_ref() {
		if (_ref != nullptr) {
			(*_ref)--;
			printfCyan("dec_ref: page table %p, ref count: %d\n", _base_addr, *_ref);
			if (*_ref <= 0) {
				// printfYellow("dec_ref: releasing page table %p (ref count reached 0)\n", _base_addr);
				// 引用计数为0，释放页表和引用计数
				if (_base_addr != 0 && !_is_global) {
					freewalk();
					_base_addr = 0;
				}
				delete _ref;
				_ref = nullptr;
			}
		} else {
			panic("dec_ref: warning - trying to decrease ref count on page table %p with null ref pointer\n", _base_addr);
		}
	}

	int PageTable::get_ref_count() {
		return _ref ? *_ref : 0;
	}

	void PageTable::share_from(const PageTable& other) {
		// 首先释放当前页表的引用
		dec_ref();
		
		// 共享另一个页表
		_base_addr = other._base_addr;
		_ref = other._ref;
		_is_global = other._is_global;
		
		// 增加共享页表的引用计数
		if (_ref != nullptr) {
			(*_ref)++;
			printfCyan("share_from: sharing page table %p, new ref count: %d\n", _base_addr, *_ref);
		} else {
			panic("share_from: warning - sharing page table %p with null ref pointer\n", _base_addr);
		}
	}
	*/
	void PageTable::print_page_table()
	{
		printfRed("PageTable: %p\n", _base_addr);
		for (int i = 0; i < 512; i++)
		{
			Pte pte = get_pte(i);
			printfRed("PTE[%d]: %p, pte2pa: %p\n", i, pte.get_data(), pte.pa());
		}
	}
	Pte PageTable::walk(uint64 va, bool alloc)
	{
		// printfRed( "walk: va=0x%x, alloc=%d\n", va, alloc );
		if (!_is_global)
		{
			printfYellow("walk: pagetable not global");
			return Pte();
		}
		// if ( va >= vml::vm_end )
		// log_panic( "va out of bounds" );

		PageTable pt;
		pt.set_base(_base_addr);
		Pte pte;

		if (debug_trace_walk)
			printf("[walk trace] 0x%x : ", va);
		uint64 pg_num;

		// search in level-3
		// printfYellow( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pgd_num(va);
		pte = pt.get_pte(pg_num);
		if (debug_trace_walk)
			printf("0x%x->", pte.get_data());
		if (!_walk_to_next_level(pte, alloc, pt))
		{
			// printfRed("walk pgd to pud fail, va=%p, pgd-base=%p, pgd-base=%p\n", va, _base_addr,
			// 		  pt._base_addr);
			return Pte();
		}

		// search in level-2
		// printfGreen( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pud_num(va);
		pte = pt.get_pte(pg_num);
		if (debug_trace_walk)
			printf("0x%x->", pte.get_data());
		if (!_walk_to_next_level(pte, alloc, pt))
		{
			// printfRed("walk pud to pmd fail, va=%p, pgd-base=%p, pud-base=%p\n", va, _base_addr,
			// 		  pt._base_addr);
			return Pte();
		}
		// search in level-1
		// printfCyan( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pmd_num(va);
		pte = pt.get_pte(pg_num);
		if (debug_trace_walk)
			printf("0x%x->", pte.get_data());
		if (!_walk_to_next_level(pte, alloc, pt))
		{
			// printfRed("walk pmd to pt fail, va=%p, pgd-base=%p, pmd-base=%p\n", va, _base_addr,
			// 		  pt._base_addr);
			return Pte();
		}

		// printfBlue( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pt_num(va);
		pte = pt.get_pte(pg_num);
		if (debug_trace_walk)
			printf("0x%x\n", pte.get_data());
		// printfMagenta( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		return pte;
	}

	void *PageTable::walk_addr(uint64 va)
	{
		uint64 pa;

		if (va >= MAXVA)
			return 0;

		Pte pte = walk(va, false /* alloc */);
		if (pte._data_addr == nullptr)
			return nullptr;
		if (!pte.is_valid())
			return nullptr;
		if (pte.is_super_plv())
		{
			Info_R("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
			return nullptr;
		}
		pa = (uint64)pte.pa();
		pa |= va & (PGSIZE - 1);
		return (void *)pa;
	}

	void PageTable::freewalk()
	{ // pte num is 4096 / 8 = 512 in pgtable
		// // 检查引用计数，只有引用计数为0或1时才真正释放
		// if (_ref != nullptr && *_ref > 1) {
		// 	panic("freewalk: page table %p still has %d references, not freeing\n", _base_addr, *_ref);
		// 	return;
		// }

		printfYellow("freewalk: freewalk page table %p\n", _base_addr);
		for (uint i = 0; i < 512; i++)
		{
			Pte next_level = get_pte(i);
			// printfMagenta("freewalk: pte[%d]: %p, pte2pa: %p\n", i, next_level.get_data(), next_level.pa());
			Pte _pte=next_level;
			// printfRed("freewalk: pte[%d]: %p, pte2pa: %p\n", i, _pte.get_data(), _pte.pa());
			bool pte_valid = _pte.is_valid();
			bool pte_leaf = !_pte.is_dir_page();
			// printfCyan("freewalk: pte[%d]: %p, pte2pa: %p\n", i, _pte.get_data(), _pte.pa());
			// if ( _pte.is_valid() && !_pte.is_leaf() )      // PGT -> PTE -> _pte
			if (pte_valid && !pte_leaf)
			{ //  get_pte_addr
				// this PTE is points to a lower-level page table
				// printfBlue("freewalk: pte[%d]: %p, pte2pa: %p\n", i, _pte.get_data(), _pte.pa());
				PageTable child;
				child.set_base(to_vir((uint64)_pte.pa()));
				child.freewalk();
				reset_pte_data(i);
			}
			else if (pte_valid)
			{
				// panic("freewalk: leaf");
			}
		}
		k_pmm.free_page((void *)_base_addr);
	}
	ulong PageTable::kwalk_addr(uint64 va)
	{
		uint64 pa;

		// if ( va >= vml::vm_end )
		// return 0;

		Pte pte = walk(va, false /* alloc */);
		if (pte.is_null())
			return 0;
		if (!pte.is_valid())
			return 0;

		pa = (uint64)PTE2PA(pte.get_data());
		pa |= va & (PGSIZE - 1);
		return pa;
	}
	uint64 PageTable::dir3_num(uint64 va)
	{
		return (va & PageEnum::dir3_vpn_mask) >> PageEnum::dir3_vpn_shift;
	}
	uint64 PageTable::dir2_num(uint64 va)
	{
		return (va & PageEnum::dir2_vpn_mask) >> PageEnum::dir2_vpn_shift;
	}
	uint64 PageTable::dir1_num(uint64 va)
	{
		return (va & PageEnum::dir1_vpn_mask) >> PageEnum::dir1_vpn_shift;
	}
	uint64 PageTable::pt_num(uint64 va)
	{
		return (va & PageEnum::pt_vpn_mask) >> PageEnum::pt_vpn_shift;
	}

	// ---------------- private helper functions ----------------

	bool PageTable::_walk_to_next_level(Pte pte, bool alloc, PageTable &pt)
	{
		if (pte.is_valid())
		{
			pt.set_base(to_vir((uint64)pte.pa()));
		}
		else
		{
			if (!alloc)
			{
				// printfRed("try to walk to next level but next level page table is not alloced.\n");
				return false;
			}
			void *page_addr = k_pmm.alloc_page();
			if (page_addr == 0)
			{
				Info_R("physical page alloc failed.");
				return false;
			}
			k_pmm.clear_page(page_addr);
			pt.set_base((uint64)page_addr);
			pte.set_data(page_round_down(to_phy((ulong)page_addr)) |
						 Pte::map_dir_page_flags());
		}
		return true;
	}
} // namespace mem
#endif
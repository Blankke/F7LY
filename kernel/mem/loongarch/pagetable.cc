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
		if ( !_is_global )
		{
			printfYellow( "walk: pagetable not global" );
			return Pte();
		}
		// if ( va >= vml::vm_end )
		// log_panic( "va out of bounds" );

		PageTable pt;
		pt.set_base( _base_addr );
		Pte pte;

		if ( debug_trace_walk ) printf( "[walk trace] 0x%x : ", va );
		uint64 pg_num;


		// search in level-3
		// printfYellow( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pgd_num( va );
		pte	   = pt.get_pte( pg_num );
		if ( debug_trace_walk ) printf( "0x%x->", pte.get_data() );
		if ( !_walk_to_next_level( pte, alloc, pt ) )
		{
			panic( "walk pgd to pud fail, va=%p, pgd-base=%p, pgd-base=%p", va, _base_addr,
					   pt._base_addr );
			return Pte();
		}
#endif
		// search in level-2
		// printfGreen( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pud_num( va );
		pte	   = pt.get_pte( pg_num );
		if ( debug_trace_walk ) printf( "0x%x->", pte.get_data() );
		if ( !_walk_to_next_level( pte, alloc, pt ) )
		{
			panic( "walk pud to pmd fail, va=%p, pgd-base=%p, pud-base=%p", va, _base_addr,
					   pt._base_addr );
			return Pte();
		}
		// search in level-1
		// printfCyan( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pmd_num( va );
		pte	   = pt.get_pte( pg_num );
		if ( debug_trace_walk ) printf( "0x%x->", pte.get_data() );
		if ( !_walk_to_next_level( pte, alloc, pt ) )
		{
			panic( "walk pmd to pt fail, va=%p, pgd-base=%p, pmd-base=%p", va, _base_addr,
					   pt._base_addr );
			return Pte();
		}

		// printfBlue( "walk: va=0x%x, pgd-base=0x%x, pud-base=0x%x, pmd-base=0x%x, pt-base=0x%x\n",
		// 			   va, _base_addr, pt._base_addr, pt._base_addr, pt._base_addr );
		pg_num = pt_num( va );
		pte	   = pt.get_pte( pg_num );
		if ( debug_trace_walk ) printf( "0x%x\n", pte.get_data() );
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
		if ( pte.is_super_plv() )
		{
			Info_R("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
			return nullptr;
		}
		pa = (uint64)pte.pa();
		return (void *)pa;
	}

	void PageTable::freewalk()
	{ // pte num is 4096 / 8 = 512 in pgtable
		for (uint i = 0; i < 512; i++)
		{
			Pte next_level = get_pte(i);
			Pte _pte((pte_t *)next_level.pa());
			bool pte_valid = _pte.is_valid();
			bool pte_leaf = _pte.is_leaf();
			// if ( _pte.is_valid() && !_pte.is_leaf() )      // PGT -> PTE -> _pte
			if (pte_valid && !pte_leaf)
			{ //  get_pte_addr
				// this PTE is points to a lower-level page table
				PageTable child;
				child.set_base((uint64)_pte.pa());
				child.freewalk();
				reset_pte_data(i);
			}
			else if (pte_valid)
			{
				panic("freewalk: leaf");
			}
		}
		k_pmm.free_page((void *)_base_addr);
	}
		ulong PageTable::kwalk_addr( uint64 va )
	{
		uint64 pa;

		// if ( va >= vml::vm_end )
		// return 0;

		Pte pte = walk( va, false /* alloc */ );
		if ( pte.is_null() ) return 0;
		if ( !pte.is_valid() ) return 0;

		pa	= (uint64) PTE2PA(pte.get_data());
		pa |= va & ( PGSIZE - 1 );
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
				panic("try to walk to next level but next level page table is not alloced.");
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
			pte.set_data( page_round_down( to_phy( (ulong) page_addr ) ) |
						  Pte::map_dir_page_flags() );
		}
		return true;
	}
} // namespace mem

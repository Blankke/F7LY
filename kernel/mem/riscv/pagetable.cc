#include "pagetable.hh"
#include "memlayout.hh"
#include "platform.hh"
#include "klib.hh"
#include "printer.hh"
#include "physical_memory_manager.hh"
namespace mem
{
    PageTable k_pagetable;
    // walk函数用于在页表中查找一个虚拟地址对应的物理地址
    Pte PageTable::walk(uint64 va, bool alloc)
    {
        // printfRed("walk: %p\n", va);
        // for (int level = 2; level > 0; level--)
        // {
        //     Pte pte = get_pte(PX(level, va));
        //     if (pte.is_valid())
        //     {
        //         PageTable pt;
        //         pt.set_base(PTE2PA(pte.get_data()));
        //         return pt.get_pte(PX(0, va));
        //     }
        //     else
        //     {
        //         PageTable pt;
        //         pt.set_base((uint64)k_pmm.alloc_page());
        //         if (!alloc || pt.get_base() == 0)
        //         {
        //             return Pte(0);
        //         }
        //         memset((void *)pt.get_base(), 0, PGSIZE);
        //         pte.set_data(PA2PTE(pt.get_base()));
        //         pte.set_valid();
        //         ((pte_t*)_base_addr)[PX(level,va)] = pte.get_data();
        //     }
        // }

        // for (int level = 2; level > 0; level--)
        // {
        //     pte_t *pte = get_pte(PX(level, va)).get_data_addr();
        //     PageTable pagetable;
        //     if (*pte & PTE_V)
        //     {

        //         pagetable.set_base((pagetable_t) PTE2PA(*pte));
        //     }
        //     else
        //     {
        //         if (!alloc || (pagetable.get_base() = (pde_t *)kalloc()) == 0)
        //             return 0;
        //         memset(pagetable, 0, PGSIZE);
        //         *pte = PA2PTE(pagetable) | PTE_V;
        //     }
        // }
        // return get_pte(PX(0, va));

        PageTable current_pt = *this; // 保存当前页表副本（不修改原对象）
        Pte return_pte;

        for (int level = 2; level > 0; level--)
        {
            const uint64 index = PX(level, va);
            Pte pte = current_pt.get_pte(index);
            //DEBUG:
            // if(va == KERNBASE)
            //     printfBlue("set前,level: %d, index: %d, pte: %p, pteaddr: %p, pte2pa:%p\n", level, index, pte.get_data(),pte.get_data_addr(), PTE2PA(pte.get_data()));

            if (pte.is_valid())
            {
                // 有效PTE：创建新页表对象指向下一级
                PageTable next_level;
                next_level.set_base(PTE2PA(pte.get_data()));
                current_pt = next_level;

                // // DEBUG:
                // if (va == KERNBASE)
                //     printfBlue
            }
            else
            {
                // 处理分配逻辑
                if (!alloc)
                    return Pte(nullptr);

                uint64 new_base = (uint64)k_pmm.alloc_page();
                if (new_base == 0)
                    return Pte(nullptr);

                // 初始化新页表
                PageTable new_pt;
                new_pt.set_base(new_base);
                memset((void *)new_base, 0, PGSIZE);
                // printfBlue("下一级页表基地址：%p\n", new_base);

                // 将新页表地址写入当前PTE（注意原子操作）
                // printfBlue("准备set\n");
                pte.set_data(PA2PTE(new_base) | PTE_V);
                // printfBlue("set后,level: %d, index: %d, pte: %p, pteaddr: %p, pte2pa:%p\n", level, index, pte.get_data(), pte.get_data_addr(), PTE2PA(pte.get_data()));

                if(level == 1)
                {
                    // current_pt.print_page_table();
                    // printfBlue("new_pt: %p\n", new_pt.get_base());
                    // new_pt.print_page_table();
                }

                // 更新当前页表指向新分配的层级
                current_pt = new_pt;
            }
        }

        // 返回最终层级的PTE
        return_pte.set_addr((uint64*)(current_pt.get_base() + 8*PX(0, va)));
        // printf("return_pte_addr: %p, 对应的va为：%p\n", (uint64 *)(current_pt.get_base() + 8 * PX(0, va)), va);
        // return_pte.set_data(PTE2PA(current_pt.get_base()) | PTE_V);

        return return_pte;
        // return current_pt.get_pte(PX(0, va)).get_data_addr();
    }

    void *PageTable::walk_addr(uint64 va)
    {
        uint64 pa;

        // if ( va >= vml::vm_end )
        // 	return 0;

        Pte pte = walk(va, false /* alloc */);
        if (pte._data_addr == nullptr)
            return nullptr;
        if (!pte.is_valid())
            return nullptr;
        if (pte.is_user() == 0)
        {
            printfCyan("try to walk-addr( k-pt, %p ). nullptr will be return.\n", va);
            return nullptr;
        }
        pa = (uint64)pte.pa();
        pa |= va & (PGSIZE - 1);
        return (void *)pa;
    }

    void PageTable::freewalk()
    {
        for (uint i = 0; i < 512; i++)
        {
            Pte pte = get_pte(i);
            if (pte.is_valid() && pte.get_flags() != 0)
            {
                PageTable child;
                child.set_base(PTE2PA(pte.get_data()));
                child.freewalk();
                reset_pte_data(i);
            }
            else if (pte.is_valid())
            {
                panic("freewalk: leaf");
            }
        }
        k_pmm.free_page((void *)(get_base()));
    }

    Pte PageTable::kwalkaddr(uint64 va)
    {
        // uint64 off = va % PGSIZE;
        Pte pte;
        // uint64 pa;

        pte = k_pagetable.walk(va, false);

        if (pte.get_data() == 0)
        {
            printfRed("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
            return Pte(0);
        }
        return pte;
    }

	ulong PageTable::kwalk_addr( uint64 va )
	{
		uint64 pa;

		// if ( va >= vml::vm_end )
		// return 0;

		Pte pte = k_pagetable.walk( va, false /* alloc */ );
		if ( pte.is_null() ) return 0;
		if ( !pte.is_valid() ) return 0;

		pa	= PTE2PA(pte.get_data());
		pa |= va & ( PGSIZE - 1 );
		return pa;
	}

    uint64 PageTable::dir_num(int level, uint64 va)
    {
        return PX(level, va);
    }

    void PageTable::print_page_table()
    {
        printfRed("PageTable: %p\n", _base_addr);
        for (int i = 0; i < 512; i++)
        {
            Pte pte = get_pte(i);
            printfRed("PTE[%d]: %p, pte2pa: %p\n", i, pte.get_data(), pte.pa());
        }
    }
}
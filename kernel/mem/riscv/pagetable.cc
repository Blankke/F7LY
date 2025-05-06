#include "pagetable.hh"
#include "memlayout.hh"
#include "platform.hh"
#include "kalloc.hh"
#include "klib.hh"
#include "printer.hh"
namespace mem
{
    PageTable k_pagetable;
    Pte PageTable::walk(uint64 va, bool alloc)
    {
        for (int level = 2; level > 0; level--)
        {
            Pte pte = get_pte(PX(level, va));
            if (pte.is_valid())
            {
                PageTable pt;
                pt.set_base(PTE2PA(pte.get_data()));
                return pt.get_pte(PX(0, va));
            }
            else
            {
                PageTable pt;
                pt.set_base((uint64)KAlloc::alloc());
                if (!alloc || pt.get_base() == 0)
                {
                    return Pte(0);
                }
                memset((void *)pt.get_base(), 0, PGSIZE);
                pte.set_data(PA2PTE(pt.get_base()));
                pte.set_valid();
                ((pte_t*)_base_addr)[PX(level,va)] = pte.get_data();
            }
        }
        return get_pte(PX(0, va));
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
            printfRed("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
            return nullptr;
        }
        pa = (uint64)pte.pa();
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
        KAlloc::free((void *)(get_base()));
    }

    Pte PageTable::kwalkaddr(uint64 va)
    {
        // uint64 off = va % PGSIZE;
        Pte pte;
        // uint64 pa;

        pte = k_pagetable.walk(va, false);

        if(pte.get_data() == 0)
        {
            printfRed("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
            return Pte(0);
        }
        return pte;
    }

    uint64 PageTable::dir_num(int level, uint64 va)
    {
        return PX(level, va);
    }
}
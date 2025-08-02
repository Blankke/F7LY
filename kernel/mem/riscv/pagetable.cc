#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined (LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "memlayout.hh"
#include "platform.hh"
#include "klib.hh"
#include "printer.hh"
#include "physical_memory_manager.hh"
namespace mem
{
    PageTable k_pagetable;

    // 阶段1注释：移除分散的页表引用计数管理，统一使用ProcessMemoryManager
    /*
    // 引用计数管理实现
    void PageTable::init_ref() {
        if (_ref == nullptr && _base_addr != 0) {
            _ref = new int(1); // 初始引用计数为1
            printfCyan("init_ref: initialized page table %p with ref count: 1\n", _base_addr);
        } else if (_ref != nullptr) {
            printfYellow("init_ref: page table %p already has ref count: %d\n", _base_addr, *_ref);
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
            // DEBUG:
            //  if(va == KERNBASE)
            //      printfBlue("set前,level: %d, index: %d, pte: %p, pteaddr: %p, pte2pa:%p\n", level, index, pte.get_data(),pte.get_data_addr(), PTE2PA(pte.get_data()));

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

                if (level == 1)
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
        return_pte.set_addr((uint64 *)(current_pt.get_base() + 8 * PX(0, va)));
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
        // // 检查引用计数，只有引用计数为0或1时才真正释放
        // if (_ref != nullptr && *_ref > 1) {
        //     panic("freewalk: page table %p still has %d references, not freeing\n", _base_addr, *_ref);
        //     return;
        // }

        for (uint i = 0; i < 512; i++)
        {
            Pte pte = get_pte(i);
            if (pte.is_valid() && (!pte.is_leaf())) // 这也抄错了吧，原来写的pte.get_flags() != 0是什么鬼，comment by @gkq
            {
                PageTable child;
                child.set_base(PTE2PA(pte.get_data()));
                child.freewalk();
                reset_pte_data(i);
            }
            else if (pte.is_valid())
            {
                ///@todo!!!!!!!!!!!目前无视风险，继续访问！！！！！！
                // printfRed("freewalk: leaf pte[%d]: %p, pte2pa: %p\n", i, pte.get_data(), pte.pa());
                // 打印当前页表项va
                // printfRed("freewalk: leaf pte[%d]: %p, pte2pa: %p, va: %p\n", i, pte.get_data(), pte.pa(), (void *)(get_base() + 8 * i));
                // panic("freewalk: leaf");
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

        if (pte.is_null() || pte.get_data() == 0)
        {
            printfRed("try to walk-addr( k-pt, %p ). nullptr will be return.", va);
            return Pte(0);
        }
        return pte;
    }

    ulong PageTable::kwalk_addr(uint64 va)
    {
        uint64 pa;

        // if ( va >= vml::vm_end )
        // return 0;

        Pte pte = k_pagetable.walk(va, false /* alloc */);
        if (pte.is_null())
            return 0;
        if (!pte.is_valid())
            return 0;

        pa = PTE2PA(pte.get_data());
        pa |= va & (PGSIZE - 1);
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

    void PageTable::freewalk_mapped()
    {
#ifdef RISCV
        uint pte_cnt = PGSIZE / sizeof(pte_t);
        for (uint i = 0; i < pte_cnt; i++)
        {
            Pte next_level = get_pte(i);
            Pte _pte((pte_t *)next_level.pa());
            bool pte_dir = !_pte.is_leaf();
            if (pte_dir) // 如果pte指向一个页表，则递归释放
            {
                // this PTE is points to a lower-level page table
                PageTable child;
                child.set_base((uint64)_pte.pa());
                child.freewalk();
                reset_pte_data(i);
            }
            else if (_pte.is_valid()) // 如果pte指向一个有效的物理页面，则将页面释放
            {
                k_pmm.free_page((void *)_pte.pa());
            }
        }
        k_pmm.free_page((void *)(get_base()));
#elif defined(LOONGARCH)
        uint pte_cnt = PGSIZE / sizeof(pte_t);
        for (uint i = 0; i < pte_cnt; i++)
        {
            Pte next_level = get_pte(i);
            Pte _pte((pte_t *)next_level.to_pa());
            bool pte_dir = !_pte.is_leaf();
            if (pte_dir) // 如果pte指向一个页表，则递归释放
            {
                // this PTE is points to a lower-level page table
                PageTable child;
                child.set_base(k_mem->to_vir(_pte.to_pa()));
                child.freewalk();
                reset_pte_data(i);
            }
            else if (_pte.is_valid()) // 如果pte指向一个有效的物理页面，则将页面释放
            {
                k_pmm.free_pages((void *)hsai::k_mem->to_vir(_pte.to_pa()));
            }
        }
        k_pmm.free_page((void *)(get_base()));
#endif
    }


    void PageTable::print_all_map()
    {
        printf("print pagetable %p\n", get_base());
        _vmprint(1, 0);  // 从虚拟地址0开始
    }

    void PageTable::_vmprint(int level, uint64 va_base)
    {
        // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++)
    {
        Pte pte = get_pte(i);
        // PTE_V is a flag for whether the page table is valid
        if (pte.is_valid())
        {
            for (int j = 0; j < level; j++)
            {
                if (j) printf(" ");
                printf("..");
            }
            uint64 child = (uint64)pte.pa();
            
            if (!pte.is_leaf())
            {
                printf("%d: pte %p pa %p\n", i, pte.get_data(), child);
                // this PTE points to a lower-level page table.
                PageTable child_pt;
                child_pt.set_base(child);
                // 计算下一级的虚拟地址基址
                uint64 next_va_base = va_base | ((uint64)i << (12 + 9 * (2 - level)));
                child_pt._vmprint(level + 1, next_va_base);
            }
            else
            {
                // 对于leaf页面，计算完整的虚拟地址
                uint64 va = va_base | ((uint64)i << 12);
                printf("%d: pte %p pa %p va %p\n", i, pte.get_data(), child, (void*)va);
            }
        }
    }
    }
}
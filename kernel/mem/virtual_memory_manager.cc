#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "pagetable.hh"
#include "memlayout.hh"
#include "platform.hh"

namespace mem
{
    VirtualMemoryManager k_vmm;
    

    // uint64 VirtualMemoryManager::kstack_vm_from_gid(uint gid)
    // {
    //     if (gid >= pm::num_process)
    //         panic("vmm: invalid gid");
    //     return (vml::vm_trap_frame - (((gid + 1) * 2) << PGSIZE_shift));
    // }

    void VirtualMemoryManager::init(const char *lock_name)
    {
#ifdef RISCV
        _virt_mem_lock.init(lock_name);

        k_pagetable.set_global();
        
        uint64 addr = (uint64)PhysicalMemoryManager::alloc_page();
        if (addr == 0)
            panic("vmm init failed");
        k_pmm.clear_page((void *)addr);
        k_pagetable.set_base(addr);

        //TODO
        // for (pm::Pcb &pcb : pm::k_proc_pool)
        // {
        //     pcb.map_kstack(k_pagetable);
        // }

        // 设置satp，对应龙芯应该设置pgdl，pgdh，stlbps，asid，tlbrehi，pwcl，pwch,
        // 并且invtlb 0x0,$zero,$zero;
        // question: 为什么xv6的MAKE_SATP没有设置asid
        sfence_vma();
        w_satp(MAKE_SATP(k_pagetable.get_base()));
        sfence_vma();
#endif 
    }

    // 根据传入的 flags 标志，生成对应的页表权限（perm）值
    bool VirtualMemoryManager::map_pages(PageTable &pt, uint64 va, uint64 size, uint64 pa, uint64 flags)
    {
#ifdef RISCV
        uint64 a, last;
        Pte pte;

        if (size == 0)
            panic("mappages: size");

        a = PGROUNDDOWN(va);
        last = PGROUNDDOWN(va + size - 1);
        for (;;)
        {
            pte = pt.walk(a, /*alloc*/ true);
            if (pte.is_null())
            {
                printfRed("walk failed");
                return false;
            }
            if (pte.is_valid())
                panic("mappages: remap");

            pte.set_data(PGROUNDDOWN(
                                riscv::virt_to_phy_address(pa)) |
                                flags |
                                riscv::PteEnum::pte_valid_m);
            if (a == last)
                break;
            a += PGSIZE;
            pa += PGSIZE;
        }
        return true;
#endif
    }

    uint64 VirtualMemoryManager::vmalloc(PageTable &pt, uint64 old_sz, uint64 new_sz, uint64 flags)
    {
#ifdef RISCV
        void *mem;

        if (new_sz < old_sz)
            return old_sz;

        old_sz = PGROUNDUP(old_sz);
        for (uint64 a = old_sz; a < new_sz; a += PGSIZE)
        {
            mem = PhysicalMemoryManager::alloc_page();
            if (mem == nullptr)
            {
                vmdealloc(pt, a, old_sz);
                return 0;
            }
            k_pmm.clear_page(mem);
            if (map_pages(pt, a, PGSIZE, (uint64)mem, 
                            riscv::PteEnum::pte_readable_m | flags) == false)
            {
                k_pmm.free_page(mem);
                vmdealloc(pt, a, old_sz);
                return 0;
            }
        }
        return new_sz;
#endif
    }

    uint64 VirtualMemoryManager::vmdealloc(PageTable &pt, uint64 old_sz, uint64 new_sz)
    {
        if (new_sz >= old_sz)
            return old_sz;

        if (PGROUNDUP(new_sz) < PGROUNDUP(old_sz))
        {
            int npages = (PGROUNDUP(old_sz) - PGROUNDUP(new_sz)) / PGSIZE;
            vmunmap(pt, PGROUNDUP(new_sz), npages, true);
        }

        return new_sz;
    }

    int VirtualMemoryManager::copy_in(PageTable &pt, void *dst, uint64 src_va, uint64 len)
    {
        uint64 n, va, pa;
        char *p_dst = (char *)dst;

        while (len > 0)
        {
            va = PGROUNDDOWN(src_va);
            pa = (uint64)pt.walk_addr(va);
            if (pa == 0)
                return -1;
            n = PGSIZE - (src_va - va);
            if (n > len)
                n = len;
            memmove((void *)p_dst, (const void *)(pa + (src_va - va)), n);

            len -= n;
            p_dst += n;
            src_va = va + PGSIZE;
        }
        return 0;
    }

    int VirtualMemoryManager::copy_str_in(PageTable &pt, void *dst, uint64 src_va, uint64 max)
    {
        uint64 n, va, pa;
        int got_null = 0;
        char *p_dst = (char *)dst;

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            pa = (uint64)pt.walk_addr(va);
            if (pa == 0)
                return -1;
            n = PGSIZE - (src_va - va);
            if (n > max)
                n = max;

            char *p = (char *)(pa + (src_va - va));
            while (n > 0)
            {
                if (*p == '\0')
                {
                    *p_dst = '\0';
                    got_null = 1;
                    break;
                }
                else
                {
                    *p_dst = *p;
                }
                --n;
                --max;
                p++;
                p_dst++;
            }

            src_va = va + PGSIZE;
        }
        if (got_null)
        {
            return 0;
        }
        else
        {
            return -1;
        }
    }


    // TODO
    // uint64 VirtualMemoryManager::allocshm(PageTable &pt, uint64 oldshm, uint64 newshm, uint64 sz, void *phyaddr[pm::MAX_SHM_PGNUM])
    // {
    //     void *mem;
    //     uint64 a;

    //     if (oldshm & 0xfff || newshm & 0xfff || newshm < sz || oldshm > (vm_trap_frame - 64 * 2 * PGSIZE))
    //     {
    //         panic("allocshm: bad parameters");
    //         return 0;
    //     }
    //     a = newshm;
    //     for (int i = 0; a < oldshm; a += PGSIZE, i++)
    //     {
    //         mem = PhysicalMemoryManager::alloc_page();
    //         if (mem == nullptr)
    //         {
    //             panic("allocshm: no memory");
    //             deallocshm(pt, newshm, a);
    //             return 0;
    //         }
    //         map_pages(pt, a, PGSIZE, uint64(phyaddr[i]), loongarch::PteEnum::presence_m | loongarch::PteEnum::writable_m | loongarch::PteEnum::plv_m | loongarch::PteEnum::mat_m | loongarch::PteEnum::dirty_m);
    //         phyaddr[i] = mem;
    //         printf("allocshm: %p => %p\n", a, phyaddr[i]);
    //     }
    //     return newshm;
    // }
    // TODO
    // uint64 VirtualMemoryManager::mapshm(PageTable &pt, uint64 oldshm, uint64 newshm, uint sz, void **phyaddr)
    // {
    //     uint64 a;
    //     if (oldshm & 0xfff || newshm & 0xfff || newshm < sz || oldshm > (vm_trap_frame - 64 * 2 * PGSIZE))
    //     {
    //         panic("mapshm: bad parameters when shmmap");
    //         return 0;
    //     }
    //     a = newshm;
    //     for (int i = 0; a < oldshm; a += PGSIZE, i++)
    //     {
    //         map_pages(pt, a, PGSIZE, uint64(phyaddr[i]), loongarch::PteEnum::presence_m | loongarch::PteEnum::writable_m | loongarch::PteEnum::plv_m | loongarch::PteEnum::mat_m | loongarch::PteEnum::dirty_m);
    //         printf("mapshm: %p => %p\n", a, phyaddr[i]);
    //     }
    //     return newshm;
    // }

    // uint64 VirtualMemoryManager::deallocshm(PageTable &pt, uint64 oldshm, uint64 newshm)
    // {
    //     if (newshm <= oldshm)
    //         return oldshm;

    //     if (PGROUNDUP(newshm) > PGROUNDUP(oldshm))
    //     {
    //         int npages = PGROUNDUP(newshm) - PGROUNDUP(oldshm) / PGSIZE;
    //         vmunmap(pt, PGROUNDUP(oldshm), npages, 0);
    //     }
    //     return oldshm;
    // }

    int VirtualMemoryManager::copyout(PageTable &pt, uint64 va, const void *p, uint64 len)
    {
#ifdef RISCV
        uint64 n, a, pa;

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            Pte pte = pt.walk(a, 0);
            pa = reinterpret_cast<uint64>(pte.pa());
            if (pa == 0)
                return -1;
            n = PGSIZE - (va - a);
            if (n > len)
                n = len;
            memmove((void *)(pa + (va - a)), p, n);

            len -= n;
            p = (char *)p + n;
            va = a + PGSIZE;
        }
        return 0;
#elif defined(LOONGARCH)
        uint64 n, a, pa;

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            Pte pte = pt.walk(a, 0);
            pa = reinterpret_cast<uint64>(pte.pa());
            if (pa == 0)
                return -1;
            n = PGSIZE - (va - a);
            if (n > len)
                n = len;
            memmove((void *)((pa + (va - a)) | loongarch::qemuls2k::dmwin::win_0), p, n);

            len -= n;
            p = (char *)p + n;
            va = a + PGSIZE;
        }
        return 0;
#endif
    }

    void VirtualMemoryManager::vmunmap(PageTable &pt, uint64 va, uint64 npages, int do_free)
    {
        uint64 a;
        Pte pte;

        if ((va % PGSIZE) != 0)
            panic("vmunmap: not aligned");

        for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
        {
            if ((pte = pt.walk(a, 0)).is_null())
                panic("vmunmap: walk");
            if (!pte.is_valid())
                panic("vmunmap: not mapped");
            if (!pte.is_leaf())
                panic("vmunmap: not a leaf");
            if (do_free)
            {
                k_pmm.free_page(pte.pa());
            }
            pte.clear_data();
        }
    }

    PageTable VirtualMemoryManager::vm_create()
    {
        PageTable pt;
        pt.set_global();

        uint64 addr = (uint64)PhysicalMemoryManager::alloc_page();
        if (addr == 0)
            panic("vmm: no mem to crate vm space.");
        k_pmm.clear_page((void *)addr);
        pt.set_base(addr);

        return pt;
    }

    int VirtualMemoryManager::vm_copy(PageTable &old_pt, PageTable &new_pt, uint64 size)
    {
        Pte pte;
        uint64 pa, va;
        uint64 flags;
        void *mem;

        for (va = 0; va < size; va += PGSIZE)
        {
            if ((pte = old_pt.walk(va, 0)).is_null())
                panic("uvmcopy: pte should exist");
            if (pte.is_valid())
                panic("uvmcopy: page not valid");
            pa = (uint64)pte.pa();
            flags = pte.get_flags();
            if ((mem = mem::PhysicalMemoryManager::alloc_page()) == nullptr)
            {
                vmunmap(new_pt, 0, va / PGSIZE, 1);
                return -1;
            }
            memmove(mem, (const char *)pa, PGSIZE);
            if (map_pages(new_pt, va, PGSIZE, (uint64)mem, flags) == false)
            {
                k_pmm.free_page(mem);
                vmunmap(new_pt, 0, va / PGSIZE, 1);
                return -1;
            }
        }
        return 0;
    }

    void VirtualMemoryManager::vmfree(PageTable &pt, uint64 sz)
    {
        if (sz > 0)
            vmunmap(pt, 0, PGROUNDUP(sz) / PGSIZE, 1);
        pt.freewalk();
    }

    void VirtualMemoryManager::uvmclear(PageTable &pt, uint64 va)
    {
        Pte pte = pt.walk(va, 0);
        if (pte.is_valid())
            pte.set_data(pte.get_data() & ~riscv::PteEnum::pte_user_m);
    }

    uint64 VirtualMemoryManager::uvmalloc(PageTable &pt, uint64 oldsz, uint64 newsz,uint64 flags)
    {
        uint64 a;
        uint64 pa;

        if (newsz < oldsz) // shrink, not here
            return oldsz;
        a = PGROUNDUP(oldsz); // start from the next page

        for (a = oldsz; a < newsz; a += PGSIZE)
        {
            pa = (uint64)k_pmm.alloc_page();
            if (pa == 0)
            {
                vmfree(pt, oldsz);
                return 0;
            }
            if (!map_pages(pt, a, PGSIZE, pa,
                           riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_user_m | flags))
            {
                k_pmm.free_page((void *)pa);
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
        }
        return newsz;
    }

    uint64 VirtualMemoryManager::uvmdealloc(PageTable &pt, uint64 oldsz, uint64 newsz)
    {
        if (newsz >= oldsz)
            return oldsz;
        if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
            vmunmap(pt,
                    PGROUNDUP(newsz),
                    (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE,
                    1);
        return newsz;
    }

}
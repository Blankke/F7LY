#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "trap/loongarch/pci.h"
#include "mem.hh" // 添加mmap相关常量定义
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#include "fs/vfs/file/normal_file.hh" // 添加文件系统支持
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#include "vfs/file/normal_file.hh" // 添加文件系统支持
#endif
#include "memlayout.hh"
#include "platform.hh"
#include "printer.hh"
#include "proc/proc.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"
#include "shm/shm_manager.hh"
extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
#ifdef LOONGARCH
void tlbinit(void)
{
    asm volatile("invtlb  0x0,$zero,$zero");
    w_csr_stlbps(0xcU);
    w_csr_asid(0x0U);
    w_csr_tlbrehi(0xcU);
}
#endif
namespace mem
{
    VirtualMemoryManager k_vmm;

    uint64 VirtualMemoryManager::kstack_vm_from_global_id(uint global_id)
    {
        if (global_id >= proc::num_process)
            panic("vmm: invalid global_id");
        return KSTACK(global_id);
    }

    void VirtualMemoryManager::init(const char *lock_name)
    {

        _virt_mem_lock.init(lock_name);
        // 创建内核页表
        k_pagetable = kvmmake();
        // for(uint64 va = KERNBASE; va < (uint64)etext; va += PGSIZE)
        // {
        //     uint64 ppp= (uint64)k_pagetable.walk_addr(va);
        //     printfRed("va: %p, pa: %p\n", va, ppp);
        // }
        // TODO
        for (proc::Pcb &pcb : proc::k_proc_pool)
        {
            pcb.map_kstack(k_pagetable);
        }
#ifdef RISCV
        // 设置satp，对应龙芯应该设置pgdl，pgdh，stlbps，asid，tlbrehi，pwcl，pwch,
        // 并且invtlb 0x0,$zero,$zero;
        // question: 为什么xv6的MAKE_SATP没有设置asid

        sfence_vma();
        // printfYellow("sfence\n");
        w_satp(MAKE_SATP(k_pagetable.get_base()));
        // printfYellow("sfence\n");
        sfence_vma();
#elif defined(LOONGARCH)

        // the "pgdl" is corresponding to "satp" in riscv
        w_csr_pgdl((uint64)k_pagetable.get_base());
        // flush the tlb(tlbinit)
        tlbinit();

        w_csr_pwcl((PTEWIDTH << 30) | (DIR2WIDTH << 25) | (DIR2BASE << 20) | (DIR1WIDTH << 15) | (DIR1BASE << 10) | (PTWIDTH << 5) | (PTBASE << 0));
        w_csr_pwch((DIR4WIDTH << 18) | (DIR3WIDTH << 6) | (DIR3BASE << 0) | (PWCH_HPTW_EN << 24));

        [[maybe_unused]] uint64 crmd = r_csr_crmd();

#endif
        printfGreen("[vmm] Virtual Memory Manager Init\n");
    }

    // 根据传入的 flags 标志，生成对应的页表权限（perm）值
    bool VirtualMemoryManager::map_pages(PageTable &pt, uint64 va, uint64 size, uint64 pa, uint64 flags)
    {
        // printf("map_pages: va=0x%x, size=0x%x, pa=0x%x, flags=0x%x\n", va, size, pa, flags);

        uint64 a, last;
        Pte pte;

        if (size == 0)
            panic("mappages: size");

        a = PGROUNDDOWN(va);

        last = PGROUNDDOWN(va + size - 1);

        for (;;)
        {
            // printfMagenta("map_pages: va=0x%x, size=0x%x, pa=0x%x, flags=0x%x\n", a, size, pa, flags);
            pte = pt.walk(a, /*alloc*/ true);
            // printfCyan("walk: va=0x%x, pte_addr=%p, pte_data=%p\n", a, pte.get_data(), pte.get_data());
            // DEBUG:
            //  if(va == KERNBASE)
            //  {
            //      pte = pt.walk(a, false);
            //  }

            if (pte.is_null())
            {
                printfRed("walk failed");
                return false;
            }
            if (pte.is_valid())
                panic("mappages: remap, va=0x%x, pa=0x%x, PteData:%x", a, pa, pte.get_data());
#ifdef RISCV
            pte.set_data(PA2PTE(PGROUNDDOWN(riscv::virt_to_phy_address(pa))) |
                         flags |
                         riscv::PteEnum::pte_valid_m);
#elif defined(LOONGARCH)
            pte.set_data(PA2PTE(PGROUNDDOWN(pa)) |
                         flags |
                         loongarch::pte_valid_m);
            // printfBlue("pa: %p, pte2pa: %p\n", pa, pte.pa());
#endif
            // printfMagenta("由map_page设置的第三级pte: %p,pte_addr:%p，应该是：%p\n", pte.get_data(), pte.get_data_addr(), riscv::virt_to_phy_address(pa));
            // if (pte.get_data_addr() == (uint64*)a)
            // {

            // }
            if (a == last)
                break;
            a += PGSIZE;
            pa += PGSIZE;
        }
        // printfMagenta("map finish for cycle\n");
        return true;
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
#elif defined(LOONGARCH)
        void *mem;

        if (new_sz < old_sz)
            return old_sz;

        old_sz = PGROUNDUP(old_sz);
        for (uint64 a = old_sz; a < new_sz; a += PGSIZE)
        {
            mem = PhysicalMemoryManager::alloc_page();
            if (mem == nullptr)
            {
                printfRed("vmalloc: alloc_page failed\n");
                vmdealloc(pt, a, old_sz);
                return 0;
            }
            k_pmm.clear_page(mem);
            if (map_pages(pt, a, PGSIZE, (uint64)mem,
                          PTE_R | PTE_U | flags) == false)
            {
                printfRed("vmalloc: map_pages failed\n");
                k_pmm.free_page(mem);
                vmdealloc(pt, a, old_sz);
                return 0;
            }
            // printf("[vmalloc] pt mapping %p", pt.walk_addr(a));;
            // printfCyan("[vmalloc] Successfully mapped VA: %p -> PA: %p\n", a, mem);
        }
        // printfMagenta("vmalloc: old_sz: %p, new_sz: %p\n", old_sz, new_sz);
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

    /// @brief 从用户空间拷贝数据到内核空间。
    /// @param pt 当前进程的页表，用于地址转换。
    /// @param dst 目标地址（内核空间指针），拷贝到这里。
    /// @param src_va 源地址（用户虚拟地址），从这个地址读取数据。
    /// @param len 拷贝的数据长度（字节数）。
    /// @return 成功返回0，失败返回-1（如页表无法转换用户虚拟地址）。
    int VirtualMemoryManager::copy_in(PageTable &pt, void *dst, uint64 src_va, uint64 len)
    {
        uint64 n, va, pa;
        char *p_dst = (char *)dst;

        while (len > 0)
        {
            va = PGROUNDDOWN(src_va);
            pa = (uint64)pt.walk_addr(va);
#ifdef LOONGARCH
            pa = to_vir((uint64)pt.walk_addr(va));
#endif
            if (pa == 0)
            {
                printfRed("[copyin] pa ==0! walk failed\n");
                return -1;
            }
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

    int VirtualMemoryManager::copy_str_in(PageTable &pt, void *dst,
                                          uint64 src_va, uint64 max)
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
    int VirtualMemoryManager::copy_str_in(PageTable &pt, eastl::string &dst,
                                          uint64 src_va, uint64 max)
    {
        uint64 n, va, pa;
        int got_null = 0;

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            pa = (uint64)pt.walk_addr(va);
#ifdef LOONGARCH
            pa = to_vir((uint64)pt.walk_addr(va));
#endif
            if (pa == 0)
            {
                printfRed("[copy_str_in] pa ==0! walk failed\n");
                return -EFAULT;
            }
#ifdef RISCV

#elif defined(LOONGARCH)
            pa = to_vir(pa);
#endif
            n = PGSIZE - (src_va - va);
            if (n > max)
                n = max;

            char *p = (char *)(pa + (src_va - va));
            while (n > 0)
            {
                if (*p == '\0')
                {
                    got_null = 1;
                    break;
                }
                else
                {
                    dst.push_back(*p);
                }
                --n;
                --max;
                p++;
            }

            src_va = va + PGSIZE;
        }
        if (got_null)
        {
            return 0;
        }
        else
        {
            printfRed("[copy_str_in] string not null-terminated\n");
            return -36; // ENAMETOOLONG; // 返回错误码，表示字符串未以null结尾
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

    /// @brief 为VMA惰性分配页面，统一处理mmap的各种标志和权限
    /// @param pt 页表
    /// @param va 虚拟地址
    /// @param vm VMA结构指针
    /// @param access_type 访问类型：0=读取, 1=写入, 2=执行
    /// @return 成功返回0，失败返回-1
    int VirtualMemoryManager::allocate_vma_page(PageTable &pt, uint64 va, proc::vma *vm, int access_type)
    {
        // 检查VMA权限
        if (vm->prot == PROT_NONE)
        {
            printfRed("[allocate_vma_page] access to PROT_NONE page at %p\n", va);
            return -1;
        }

        // 检查访问类型权限
        switch (access_type)
        {
        case 0: // 读取
            if (!(vm->prot & PROT_READ))
            {
                printfRed("[allocate_vma_page] read access to non-readable page at %p\n", va);
                return -1;
            }
            break;
        case 1: // 写入
            if (!(vm->prot & PROT_WRITE))
            {
                printfRed("[allocate_vma_page] write access to non-writable page at %p\n", va);
                return -1;
            }
            break;
        case 2: // 执行
            if (!(vm->prot & PROT_EXEC))
            {
                printfRed("[allocate_vma_page] exec access to non-executable page at %p\n", va);
                return -1;
            }
            break;
        }

        // 构建页表项权限
        uint64 pte_flags = 0;
#ifdef RISCV
        pte_flags = riscv::PteEnum::pte_user_m; // 用户可访问
        if (vm->prot & PROT_READ)
            pte_flags |= riscv::PteEnum::pte_readable_m;
        if (vm->prot & PROT_WRITE)
            pte_flags |= riscv::PteEnum::pte_writable_m;
        if (vm->prot & PROT_EXEC)
            pte_flags |= riscv::PteEnum::pte_executable_m;
#elif defined(LOONGARCH)
        pte_flags = PTE_U | PTE_D; // 用户可访问
        if (vm->prot & PROT_READ)
            pte_flags |= PTE_R;
        if (vm->prot & PROT_WRITE)
            pte_flags |= PTE_W;
        if (vm->prot & PROT_EXEC)
            pte_flags |= PTE_X;
        pte_flags |= PTE_MAT; // 内存访问类型
#endif

        // 分配物理页面
        void *pa = k_pmm.alloc_page();
        if (pa == nullptr)
        {
            printfRed("[allocate_vma_page] alloc_page failed for va: %p\n", va);
            return -1;
        }

        // 初始化页面内容
        k_pmm.clear_page(pa);

        // 检查是否为文件映射
        fs::normal_file *vf = vm->vfile;
        if (vf != nullptr && vm->vfd != -1)
        {
            // 文件映射：从文件读取数据
            uint64 page_va = PGROUNDDOWN(va);
            int offset = vm->offset + (page_va - vm->addr);

            printfCyan("[allocate_vma_page] reading from file %s at offset %d\n",
                       vf->_path_name.c_str(), offset);

            int readbytes = vf->read((uint64)pa, PGSIZE, offset, false);
            if (readbytes < 0)
            {
                printfRed("[allocate_vma_page] file read failed\n");
                k_pmm.free_page(pa);
                return -1;
            }

            if (readbytes < PGSIZE)
            {
                printfYellow("[allocate_vma_page] partial page read (%d bytes)\n", readbytes);
            }
        }
        else
        {
            // 匿名映射：页面已通过clear_page初始化为0
            printfCyan("[allocate_vma_page] handling anonymous mapping at %p\n", va);
        }

        // 添加页面映射
        uint64 page_va = PGROUNDDOWN(va);
        if (!this->map_pages(pt, page_va, PGSIZE, (uint64)pa, pte_flags))
        {
            printfRed("[allocate_vma_page] map_pages failed\n");
            k_pmm.free_page(pa);
            return -1;
        }

        printfGreen("[allocate_vma_page] successfully mapped page at va=%p, pa=%p, pte_flags=0x%x\n",
                    page_va, pa, pte_flags);
        return 0;
    }

    /// @brief 从内核地址空间拷贝数据到用户页表映射的虚拟地址空间。
    ///
    /// 将内核中的 `len` 字节数据从指针 `p` 拷贝到用户进程页表 `pt` 所映射的虚拟地址 `va` 起始处，
    /// 自动处理跨页情况。支持mmap的惰性分配和各种保护标志。
    ///
    /// @param pt  用户进程的页表，用于解析虚拟地址。
    /// @param va  拷贝的目标虚拟地址（用户空间），可跨页。
    /// @param p   拷贝的源地址（内核空间指针）。
    /// @param len 拷贝的字节数。
    /// @return 成功返回 0；若任意一页无效或未映射，返回 -1。
    int VirtualMemoryManager::copy_out(PageTable &pt, uint64 va, const void *p, uint64 len)
    {
#ifdef RISCV
        uint64 n, a, pa;
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();

        // 之前vma如果被free了这里会直接炸, 添加一个判断
        if (!proc || !proc->_vma)
        {
            printfRed("[copy_out] VMA not present, skip copy\n");
            return -1;
        }

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            proc::vma *target_vm = nullptr;

            // 查找对应的VMA
            for (int i = 0; i < proc::NVMA; ++i)
            {
                if (proc->_vma->_vm[i].used)
                {
                    // 检查是否在当前VMA范围内
                    if (va >= proc->_vma->_vm[i].addr && va < proc->_vma->_vm[i].addr + proc->_vma->_vm[i].len)
                    {
                        target_vm = &proc->_vma->_vm[i];
                        break;
                    }
                }
            }

            Pte pte = pt.walk(a, 0);
            if ((pte.is_null() || pte.get_data() == 0) && target_vm != nullptr)
            {
                // 如果页表项无效且在VMA范围内，使用统一的页面分配逻辑
                // copy_out 是写操作，需要写权限
                if (allocate_vma_page(pt, va, target_vm, 1) != 0)
                {
                    printfRed("[copy_out] allocate_vma_page failed for va: %p\n", va);
                    return -1;
                }
                // 重新获取页表项
                pte = pt.walk(a, 0);
            }
            else if (pte.is_null() || pte.get_data() == 0)
            {
                // 如果页表项无效且不在VMA范围内，则返回错误
                printfRed("[copy_out] walk failed for va: %p\n", va);
                return -1;
            }

            pa = reinterpret_cast<uint64>(pte.pa());
            if (pa == 0)
            {
                printfRed("[copy_out] pa == 0! walk failed for va: %p\n", va);
                return -1;
            }

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
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();

        // 之前vma如果被free了这里会直接炸, 添加一个判断
        if (!proc || !proc->_vma)
        {
            printfRed("[copy_out] VMA not present, skip copy\n");
            return -1;
        }

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            proc::vma *target_vm = nullptr;

            // 查找对应的VMA
            for (int i = 0; i < proc::NVMA; ++i)
            {
                if (proc->_vma->_vm[i].used)
                {
                    // 检查是否在当前VMA范围内
                    if (va >= proc->_vma->_vm[i].addr && va < proc->_vma->_vm[i].addr + proc->_vma->_vm[i].len)
                    {
                        target_vm = &proc->_vma->_vm[i];
                        break;
                    }
                }
            }

            Pte pte = pt.walk(a, 0);
            if ((pte.is_null() || pte.get_data() == 0) && target_vm != nullptr)
            {
                // 如果页表项无效且在VMA范围内，使用统一的页面分配逻辑
                // copy_out 是写操作，需要写权限
                if (allocate_vma_page(pt, va, target_vm, 1) != 0)
                {
                    printfRed("[copy_out] allocate_vma_page failed for va: %p\n", va);
                    return -1;
                }
                // 重新获取页表项
                pte = pt.walk(a, 0);
            }
            else if (pte.is_null() || pte.get_data() == 0)
            {
                // 如果页表项无效且不在VMA范围内，则返回错误
                printfRed("[copy_out] walk failed for va: %p (not in any VMA)\n", va);
                return -1;
            }

            pa = reinterpret_cast<uint64>(pte.pa());
            if (pa == 0)
                return -1;
            n = PGSIZE - (va - a);
            if (n > len)
                n = len;
            pa = to_vir(pa);
            memmove((void *)((pa + (va - a))), p, n);

            len -= n;
            p = (char *)p + n;
            va = a + PGSIZE;
        }
        return 0;
#endif
    }

    void VirtualMemoryManager::vmunmap(PageTable &pt, uint64 va, uint64 npages, int do_free)
    {
        // printfCyan("vmunmap: va: %p, npages: %d, do_free: %d\n", va, npages, do_free);
        uint64 a;
        Pte pte;

        if ((va % PGSIZE) != 0)
            panic("vmunmap: not aligned");

        for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
        {
            if ((pte = pt.walk(a, 0)).is_null())
                continue;
            // panic("vmunmap: walk");
            if (!pte.is_valid())
                continue;
            ///@brief 这里的逻辑是，如果pte无效，则不需要释放物理页
            /// TODO: 为了mmap的懒分配，所以确实可能出现了惰性页面调用
            // panic("vmunmap: not mapped");
            if (!pte.is_leaf())
                panic("vmunmap: not a leaf");
            if (do_free)
            {
                // printfMagenta("vmunmap: free va: %p, pa: %p\n", a, pte.pa());
                k_pmm.free_page(pte.pa());
            }
            // printfMagenta("vmunmap: unmap va: %p, pa: %p\n", a, pte.pa());
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
        pt.init_ref(); // 初始化引用计数

        return pt;
    }

    int VirtualMemoryManager::vm_copy(PageTable &old_pt, PageTable &new_pt, uint64 start, uint64 size)
    {
        Pte pte;
        uint64 pa, va;
        uint64 va_end;
        uint64 flags;
        void *mem;

        if (!is_page_align(start) || !is_page_align(size))
        {
            panic("uvmcopy: start or size not page aligned");
            return -1;
        }

        va_end = start + size;

        for (va = start; va < va_end; va += PGSIZE)
        {
            if ((pte = old_pt.walk(va, false)).is_null())
            {
                continue;
                panic("uvmcopy: pte should exist for va: %p", va);
            }
            if (pte.is_valid() == 0)
                continue;
            ///@brief 这里的逻辑是，如果pte无效，则不需要释放物理页
            /// TODO: 为了mmap的懒分配，所以确实可能出现了惰性页面调用
            // panic("uvmcopy: page not valid");
            pa = (uint64)pte.pa();
            flags = pte.get_flags();

            // 检查当前虚拟地址是否属于共享内存区域
            bool is_shared = shm::k_smm.is_shared_memory_address((void *)va);

            if (is_shared)
            {
                // 对于共享内存，直接复用原物理地址，不分配新页面
                printfCyan("[vm_copy] Sharing memory for VA=%p -> PA=%p (shared memory)\n", va, pa);
                if (map_pages(new_pt, va, PGSIZE, pa, flags) == false)
                {
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return -1;
                }
            }
            else
            {
                // 对于普通内存，分配新页面并复制内容
                if ((mem = mem::PhysicalMemoryManager::alloc_page()) == nullptr)
                {
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return -1;
                }
                memmove(mem, (const char *)pa, PGSIZE);
                // printfYellow("[vm_copy] Copying memory for VA=%p -> new PA=%p (private memory)\n", va, (uint64)mem);
                if (map_pages(new_pt, va, PGSIZE, (uint64)mem, flags) == false)
                {
                    k_pmm.free_page(mem);
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return -1;
                }
            }
        }
        return 0;
    }

    void VirtualMemoryManager::uvmclear(PageTable &pt, uint64 va)
    {
        Pte pte = pt.walk(va, 0);
#ifdef RISCV
        if (pte.is_valid())
            pte.set_data(pte.get_data() & ~riscv::PteEnum::pte_user_m);
#elif defined(LOONGARCH)
        if (pte.is_valid())
            pte.set_data(pte.get_data() & ~loongarch::PteEnum::pte_plv_m); // PTE_U
#endif
    }

    uint64 VirtualMemoryManager::uvmalloc(PageTable &pt, uint64 oldsz, uint64 newsz, uint64 flags)
    {
#ifdef RISCV
        uint64 a;
        uint64 pa;

        if (newsz < oldsz) // shrink, not here
            return oldsz;

        a = PGROUNDUP(oldsz); // start from the next page
        // printfBlue("[vmalloc]  another page :%p,walk:%p\n",a,pt.walk(a,0).get_data());
        for (; a < newsz; a += PGSIZE)
        {
            pa = (uint64)k_pmm.alloc_page();
            // printfCyan("[vmalloc] alloc page: %p\n", pa);
            if (pa == 0)
            {
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
            k_pmm.clear_page((void *)pa);
            if (!map_pages(pt, a, PGSIZE, pa, riscv::PteEnum::pte_readable_m | riscv::PteEnum::pte_user_m | flags))
            {
                k_pmm.free_page((void *)pa);
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
        }
        return newsz;
#elif defined(LOONGARCH)
        /// TODO:未测试正确性
        void *mem;
        uint64 a;
        // printfCyan("[vmalloc] oldsz: %p, newsz: %p\n", oldsz, newsz);
        if (newsz < oldsz)
            return oldsz;

        oldsz = PGROUNDUP(oldsz);
        for (a = oldsz; a < newsz; a += PGSIZE)
        {
            mem = k_pmm.alloc_page();
            if (mem == 0)
            {
                // printfCyan("[vmalloc] alloc page failed, oldsz: %p, newsz: %p\n", oldsz, newsz);
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
            memset(mem, 0, PGSIZE);
            if (map_pages(pt, a, PGSIZE, (uint64)mem, flags | PTE_U | PTE_D) == 0)
            {
                // printfCyan("[vmalloc] map page failed, oldsz: %p, newsz: %p\n", oldsz, newsz);
                k_pmm.free_page(mem);
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
        }
        return newsz;
#endif
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
    void VirtualMemoryManager::kvmmap(PageTable &pt, uint64 va, uint64 pa, uint64 sz, uint64 perms)
    {
        if (map_pages(pt, va, sz, pa, perms) == false)
        {
            printf("kvmmap failed\n");
            panic("[vmm] kvmmap failed");
        }
    }

    void VirtualMemoryManager::pci_map(int bus, int dev, int func, void *pages)
    {
#ifdef LOONGARCH

        uint64 va = PCIE0_ECAM_V + ((bus << 16) | (dev << 11) | (func << 8));
        uint64 pa = PCIE0_ECAM + ((bus << 16) | (dev << 11) | (func << 8));
        map_pages(k_pagetable, va, PGSIZE, pa, PTE_MAT | PTE_W | PTE_P | PTE_D);
        static int first = 0;
        if (!first)
        {
            va = PCIE0_MMIO_V;
            pa = PCIE0_MMIO;
            map_pages(k_pagetable, va, 16 * PGSIZE, pa, PTE_MAT | PTE_W | PTE_P | PTE_D);
            first = 1;
        }

        // mappages(kernel_pagetable, ((uint64)pages) & (~(DMWIN_MASK)), 2 * PGSIZE, pages, PTE_W | PTE_P | PTE_D | PTE_MAT);

#endif
    }

    PageTable VirtualMemoryManager::kvmmake()
    {
        PageTable pt;
        pt.set_global();
        pt.set_base((uint64)k_pmm.alloc_page());
        // pt.init_ref(); // 初始化引用计数
        // printfGreen("[vmm] kvmmake alloc page success\n");
        memset((void *)pt.get_base(), 0, PGSIZE);
        // pt.print_page_table();
#ifdef RISCV
        // uart registers
        kvmmap(pt, UART0, UART0, PGSIZE, PTE_R | PTE_W);
        // printfGreen("[vmm] kvmmake uart0 success\n");
        // uint64 ppp = (uint64)pt.walk_addr(UART0);
        // printfGreen("va: %p, pa: %p\n", UART0, ppp);
        // // virtio mmio disk interface
        kvmmap(pt, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
        // printfGreen("[vmm] kvmmake virtio0 success\n");
        kvmmap(pt, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);
        // printfGreen("[vmm] kvmmake virtio1 success\n");
        // // CLINT
        kvmmap(pt, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
        // printfGreen("[vmm] kvmmake clint success\n");
        // // PLIC
        kvmmap(pt, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
        // printfGreen("[vmm] kvmmake plic success\n");
        // map kernel text executable and read-only.
        kvmmap(pt, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
        // printfGreen("[vmm] kvmmake kernel text success\n");
        // map kernel data and the physical RAM we'll make use of.
        kvmmap(pt, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);
        // printfRed("[vmm] kvmmake kernel data success\n");
        // // map the trampoline for trap entry/exit to
        // // the highest virtual address in the kernel.
        kvmmap(pt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
        // 我发现trapframe和kstack在xv6里面都没有初始化
        // 因为trampoline的位置在内核和用户页表都一样，
        // 所以他们访问的时候都是通过trampoline进行访问，没有进行映射也没有关系,
        // 所以这里不需要进行映射.
        /*实际上，proc在创建的时候会有两个函数，proc_pagetable,proc_mapstacks,
        这二者会分别映射trampoline和kstack ，我们内核的页表初始化的时候已经映射了trampoline
        这里要映射的*/

        // DEBUG:虚拟化后所有代码卡死，检查所有内核代码映射，KERNBASE到etext
        // printfBlue("etext: %p\n", etext);
        // printfBlue("KERNBASE: %p\n", KERNBASE);
        // for(uint64 va = KERNBASE; va < (uint64)etext; va += PGSIZE)
        // {
        //     uint64 ppp= (uint64)pt.walk_addr(va);
        //     printfRed("va: %p, pa: %p\n", va, ppp);
        // }

        // 初始化堆内存
        kvmmap(pt, vm_kernel_heap_start, HEAP_START, vm_kernel_heap_size, PTE_R | PTE_W);
#elif defined(LOONGARCH)
        kvmmap(pt, ((uint64)etext) & (~(DMWIN_MASK)), (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

#endif
        return pt;
    }

    uint64 VirtualMemoryManager::uvmfirst(PageTable &pt, uint64 src, uint64 sz)
    {
#ifdef RISCV
        // 动态计算需要分配的空间
        char *mem;
        printf("sz: %d\n", sz);

        // 计算程序段需要的页面数量（向上取整）
        uint64 prog_pages = PGROUNDUP(sz) / PGSIZE;
        // 总共分配两倍的页面数，低地址存程序段，高地址作栈内存
        uint64 total_pages = prog_pages * 2;
        uint64 total_size = total_pages * PGSIZE;

        printf("prog_pages: %d, total_pages: %d, total_size: %d\n", prog_pages, total_pages, total_size);

        // 分配程序段页面
        for (uint64 i = 0; i < prog_pages; i++)
        {
            mem = (char *)k_pmm.alloc_page();
            memset(mem, 0, PGSIZE);
            map_pages(pt, i * PGSIZE, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);

            // 复制程序内容
            uint64 src_offset = i * PGSIZE;
            uint64 copy_size = MIN(sz - src_offset, PGSIZE);
            if (copy_size > 0 && src_offset < sz)
            {
                memmove(mem, (void *)((uint64)src + src_offset), copy_size);
            }
        }

        // 分配栈内存页面
        for (uint64 i = prog_pages; i < total_pages; i++)
        {
            mem = (char *)k_pmm.alloc_page();
            memset(mem, 0, PGSIZE);
            // 栈内存只需要读写权限，不需要执行权限
            map_pages(pt, i * PGSIZE, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_U);
        }

        return total_size;
#elif defined(LOONGARCH)
        // 动态计算需要分配的空间
        char *mem;
        printf("sz: %d\n", sz);

        // 计算程序段需要的页面数量（向上取整）
        uint64 prog_pages = PGROUNDUP(sz) / PGSIZE;
        // 总共分配两倍的页面数，低地址存程序段，高地址作栈内存
        uint64 total_pages = prog_pages * 2;
        uint64 total_size = total_pages * PGSIZE;

        printf("prog_pages: %d, total_pages: %d, total_size: %d\n", prog_pages, total_pages, total_size);

        // 分配程序段页面
        for (uint64 i = 0; i < prog_pages; i++)
        {
            mem = (char *)k_pmm.alloc_page();
            memset(mem, 0, PGSIZE);
            map_pages(pt, i * PGSIZE, PGSIZE, (uint64)mem, PTE_V | PTE_W | PTE_R | PTE_X | PTE_MAT | PTE_PLV | PTE_D | PTE_P);

            // 复制程序内容
            uint64 src_offset = i * PGSIZE;
            uint64 copy_size = MIN(sz - src_offset, PGSIZE);
            if (copy_size > 0 && src_offset < sz)
            {
                memmove(mem, (void *)((uint64)src + src_offset), copy_size);
            }
        }

        // 分配栈内存页面
        for (uint64 i = prog_pages; i < total_pages; i++)
        {
            mem = (char *)k_pmm.alloc_page();
            memset(mem, 0, PGSIZE);
            // 栈内存只需要读写权限，不需要执行权限
            map_pages(pt, i * PGSIZE, PGSIZE, (uint64)mem, PTE_V | PTE_W | PTE_R | PTE_MAT | PTE_PLV | PTE_D | PTE_P);
        }

        return total_size;

#endif
    }

    int VirtualMemoryManager::protectpages(PageTable &pt, uint64 va, uint64 size, int perm, bool is_vma)
    {
        uint64 a, last;
        Pte pte;

        // printf("[protectpages] va: %p, size: %p, perm: %p, is_vma: %d\n", va, size, perm, is_vma);

        last = PGROUNDDOWN(va + size - 1);

        for (a = PGROUNDDOWN(va); a != last + PGSIZE; a += PGSIZE)
        {
            pte = pt.walk(a, 1);
            if (pte.is_null())
                return -1;

            // 如果页表项为空
            if (pte.get_data() == 0)
            {
                if (is_vma)
                {
                    // VMA 上下文：懒分配情况，忽略空页表项
                    continue;
                }
                else
                {
                    // 非 VMA 上下文：页表项为空是错误
                    return -1;
                }
            }

            if (pte.get_data() & PTE_V)
            {
                // 清除旧的权限位，保留其他标志位，然后设置新的权限
                uint64 old_data = pte.get_data();
                uint64 new_data = (old_data & ~(PTE_R | PTE_W | PTE_X)) | perm | PTE_V | PTE_U;
                pte.set_data(new_data);
            }
            else
                pte.set_data(pte.get_data() | PTE_U);
        }
        return 0;
    }
}
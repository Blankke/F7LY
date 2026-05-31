#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "trap/loongarch/pci.h"
#include "mem.hh" // 添加mmap相关常量定义
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "memlayout.hh"
#include "platform.hh"
#include "printer.hh"
#include "fs/vfs/vfs_ext4_ext.hh" // 添加vfs_ext_get_filesize函数
#include "proc/signal.hh"         // 添加信号处理
#include "proc/process_memory_manager.hh"
#include "proc/proc_manager.hh"   // 添加进程管理
#include "fs/lwext4/ext4_errno.hh"
#include "proc/proc.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"
#include "shm/shm_manager.hh"
#include "net/drivers/virtio_net.hh"
#include "fs/vfs/vfs_utils.hh"
extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
extern uint64 k_dtb_addr; // Defined in main.cc

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

    namespace
    {
        inline void *page_pa_to_kernel_ptr(uint64 pa)
        {
#ifdef LOONGARCH
            return reinterpret_cast<void *>(to_vir(pa));
#else
            return reinterpret_cast<void *>(pa);
#endif
        }

        inline bool ranges_overlap(uint64 lhs_addr, uint64 lhs_size, uint64 rhs_addr, uint64 rhs_size)
        {
            if (lhs_size == 0 || rhs_size == 0)
            {
                return false;
            }

            uint64 lhs_end = lhs_addr + lhs_size;
            uint64 rhs_end = rhs_addr + rhs_size;
            if (lhs_end <= lhs_addr || rhs_end <= rhs_addr)
            {
                return false;
            }

            return lhs_addr < rhs_end && rhs_addr < lhs_end;
        }

        proc::vma *find_vma_covering_va(proc::Pcb *proc, uint64 va)
        {
            if (proc == nullptr || proc->get_vma() == nullptr)
            {
                return nullptr;
            }

            for (int i = 0; i < proc::NVMA; ++i)
            {
                proc::vma *vm = &proc->get_vma()->_vm[i];
                if (vm->used && va >= vm->addr && va < vm->addr + vm->len)
                {
                    return vm;
                }
            }

            return nullptr;
        }

        bool pte_allows_user_read(Pte &pte)
        {
#ifdef RISCV
            return pte.is_valid() && pte.is_user() && pte.is_readable();
#elif defined(LOONGARCH)
            return pte.is_valid() && pte.is_user_plv() && pte.is_readable();
#else
            return false;
#endif
        }

#ifdef LOONGARCH
        uint64 loongarch_empty_pgdh_base = 0;

        inline void invalidate_loongarch_user_page_pair(uint64 va)
        {
            uint64 pair_base = va & ~((PGSIZE << 1) - 1);
            asm volatile("invtlb 0x6, $zero, %0" : : "r"(pair_base) : "memory");
        }

        void install_loongarch_empty_high_user_pagetable()
        {
            if (loongarch_empty_pgdh_base == 0)
            {
                void *pgdh = k_pmm.alloc_page();
                void *pud = k_pmm.alloc_page();
                void *pmd = k_pmm.alloc_page();
                void *pte_page = k_pmm.alloc_page();
                if (pgdh == nullptr || pud == nullptr || pmd == nullptr || pte_page == nullptr)
                {
                    panic("[vmm] alloc empty LoongArch PGDH failed");
                }

                k_pmm.clear_page(pgdh);
                k_pmm.clear_page(pud);
                k_pmm.clear_page(pmd);
                k_pmm.clear_page(pte_page);

                // 用户态访问 0xffff... 这类高半区坏地址时会走 PGDH。
                // 这里让 TLBR refill 至少能抵达一个全零叶子页，随后转成普通页无效异常，
                // 避免在缺少页表层级时无限重试同一条访存指令。
                pte_t pud_entry = PGROUNDDOWN(to_phy((ulong)pud)) | Pte::map_dir_page_flags();
                pte_t pmd_entry = PGROUNDDOWN(to_phy((ulong)pmd)) | Pte::map_dir_page_flags();
                pte_t leaf_page_entry = PGROUNDDOWN(to_phy((ulong)pte_page)) | Pte::map_dir_page_flags();

                for (int i = 0; i < 512; ++i)
                {
                    ((pte_t *)pgdh)[i] = pud_entry;
                    ((pte_t *)pud)[i] = pmd_entry;
                    ((pte_t *)pmd)[i] = leaf_page_entry;
                }

                loongarch_empty_pgdh_base = (uint64)pgdh;
            }

            w_csr_pgdh(loongarch_empty_pgdh_base);
        }
#endif

        int resolve_user_read_pa(PageTable &pt, proc::Pcb *proc, uint64 user_va, uint64 &out_pa)
        {
            uint64 page_va = PGROUNDDOWN(user_va);
            proc::vma *target_vm = find_vma_covering_va(proc, user_va);
            Pte pte = pt.walk(page_va, false);

            if ((pte.is_null() || pte.get_data() == 0) && target_vm != nullptr)
            {
                // 读取用户空间时，允许对合法 VMA 做一次按需补页，
                // 但补页后仍必须检查用户态读权限，不能把 PROT_NONE 之类的映射误当成可读。
                if (k_vmm.allocate_vma_page(pt, user_va, target_vm, 0) != 0)
                {
                    printfRed("[resolve_user_read_pa] allocate_vma_page failed for va: %p\n", user_va);
                    return -1;
                }
                pte = pt.walk(page_va, false);
            }
            else if (pte.is_null() || pte.get_data() == 0)
            {
                printfRed("[resolve_user_read_pa] walk failed for va: %p\n", user_va);
                return -1;
            }

            if (pte.is_null() || pte.get_data() == 0)
            {
                printfRed("[resolve_user_read_pa] walk still invalid after lazy allocation, va: %p\n", user_va);
                return -1;
            }

            if (!pte_allows_user_read(pte))
            {
                printfRed("[resolve_user_read_pa] unreadable user page va=%p pte=%p valid=%d pt_base=%p pid=%d tid=%d\n",
                          (void *)page_va,
                          (void *)(pte.is_null() ? 0 : pte.get_data()),
                          pte.is_valid(),
                          (void *)pt.get_base(),
                          proc ? proc->_pid : -1,
                          proc ? proc->_tid : -1);
                return -1;
            }

            out_pa = reinterpret_cast<uint64>(pte.pa());
            if (out_pa == 0)
            {
                printfRed("[resolve_user_read_pa] pa == 0 for va: %p\n", user_va);
                return -1;
            }
#ifdef LOONGARCH
            out_pa = to_vir(out_pa);
#endif
            return 0;
        }
    } // namespace

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
        install_loongarch_empty_high_user_pagetable();
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
            {
                bool is_kernel_pt = !k_pagetable.is_null() && pt.get_base() == k_pagetable.get_base();
                bool is_user_va = a < TRAPFRAME;
                if (!is_kernel_pt && is_user_va)
                {
                    proc::Pcb *cur = proc::k_pm.get_cur_pcb();
                    printfRed("[mappages] reject user remap: va=%p new_pa=%p old_pte=%p pid=%d tid=%d pt=%p\n",
                              (void *)a,
                              (void *)pa,
                              (void *)pte.get_data(),
                              cur ? cur->_pid : -1,
                              cur ? cur->_tid : -1,
                              (void *)pt.get_base());
                    return false;
                }
                panic("mappages: remap, va=0x%x, pa=0x%x, PteData:%x", a, pa, pte.get_data());
            }
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
            // LoongArch 用户态普通页默认应保持可缓存，避免 ELF/堆/BSS 上的
            // ll/sc 原子在数据页上长期失败，表现成 pthread 类用例卡在用户态自旋。
            uint64 pte_flags = PTE_R | PTE_U | PTE_MAT | flags;
            if (map_pages(pt, a, PGSIZE, (uint64)mem, pte_flags) == false)
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
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();

        while (len > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, proc, src_va, pa) != 0)
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

    int VirtualMemoryManager::copy_str_in(PageTable &pt, void *dst,
                                          uint64 src_va, uint64 max)
    {
        uint64 n, va, pa;
        int got_null = 0;
        char *p_dst = (char *)dst;
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, proc, src_va, pa) != 0)
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

        // printfCyan("[copy_str_in] src_va: %p, max: %d\n", src_va, max);
        uint64 n, va, pa;
        int got_null = 0;
        proc::Pcb *proc = proc::k_pm.get_cur_pcb();

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, proc, src_va, pa) != 0)
                return -EFAULT;
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
        uint64 page_va = PGROUNDDOWN(va);

        // 线程并发 fault 同一页时，另一个线程可能已经先一步把叶子 PTE 补好了。
        // 这类场景不应该升级成 remap panic，而应该把当前 fault 视为“已经有人补完页”。
        auto reuse_existing_mapping_if_ready = [&]() -> bool {
            Pte existing_pte = pt.walk(page_va, false);
            if (existing_pte.is_null() || !existing_pte.is_valid())
            {
                return false;
            }

#ifdef RISCV
            bool user_ok = existing_pte.is_user();
#elif defined(LOONGARCH)
            bool user_ok = existing_pte.is_user_plv();
#endif
            bool access_ok = false;
            switch (access_type)
            {
            case 0:
                access_ok = existing_pte.is_readable();
                break;
            case 1:
                access_ok = existing_pte.is_writable();
                break;
            case 2:
                access_ok = existing_pte.is_executable();
                break;
            default:
                access_ok = false;
                break;
            }

            if (user_ok && access_ok)
            {
#ifdef LOONGARCH
                uint64 repaired_pte = existing_pte.get_data();
                bool need_repair = false;
                if (!existing_pte.is_present())
                {
                    repaired_pte |= PTE_P;
                    need_repair = true;
                }
                if (access_type == 1 && !existing_pte.is_dirty())
                {
                    repaired_pte |= PTE_D;
                    need_repair = true;
                }
                if (need_repair)
                {
                    existing_pte.set_data(repaired_pte);
                    invalidate_loongarch_user_page_pair(page_va);
                }
#endif
                return true;
            }

            if (user_ok)
            {
                printfRed("[allocate_vma_page] existing mapping lacks requested permission va=%p access=%d pte=%p\n",
                          (void *)page_va, access_type, (void *)existing_pte.get_data());
            }
            return false;
        };

        // LoongArch 上可能会先以 fault 形式把“已经存在的用户页”带进来，
        // 比如 clone 子任务第一次碰到刚复制好的 guarded stack。
        // 这时如果继续走 map_pages()，就会把正常可恢复的 fault 升级成 remap panic。
        // 因此先检查叶子 PTE；若映射已经存在且权限满足，直接复用即可。
        if (reuse_existing_mapping_if_ready())
        {
            return 0;
        }

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
        {
            pte_flags |= riscv::PteEnum::pte_readable_m;
        }
        if (vm->prot & PROT_WRITE)
        {
            pte_flags |= riscv::PteEnum::pte_writable_m;
            pte_flags |= riscv::PteEnum::pte_readable_m;
        }
        if (vm->prot & PROT_EXEC)
        {
            pte_flags |= riscv::PteEnum::pte_executable_m;
        }
#elif defined(LOONGARCH)
        pte_flags = PTE_U | PTE_D | PTE_P; // 用户可访问，且页已实际驻留
        if (vm->prot & PROT_READ)
            pte_flags |= PTE_R;
        if (vm->prot & PROT_WRITE)
            pte_flags |= PTE_W;
        if (vm->prot & PROT_EXEC)
            pte_flags |= PTE_X;
        pte_flags |= PTE_MAT; // 内存访问类型
#endif

        // 共享段后端在 MAP_SHARED / fork 后的缺页场景下，不应该重新分配私有物理页，
        // 否则会把“共享映射”错误降级成私有页，还会在 unlink 后继续依赖原始文件路径。
        // 正确做法是直接把共享段已经分配好的物理页重新映射进当前页表。
        if (vm->backing_kind == proc::VMA_BACKING_SHM && vm->backing_shmid >= 0)
        {
            shm::shm_segment seg = shm::k_smm.get_seg_info(vm->backing_shmid);
            if (seg.shmid < 0)
            {
                printfRed("[allocate_vma_page] invalid shared backing shmid=%d for va=%p\n",
                          vm->backing_shmid, va);
                return -1;
            }

            uint64 backing_start = PGROUNDDOWN(vm->backing_base != 0 ? vm->backing_base : vm->addr);
            uint64 page_offset = page_va - backing_start;
            if (vm->vfile != nullptr)
            {
                fs::Kstat st;
                int size_result = vfs_fstat(vm->vfile, &st);
                if (size_result != EOK)
                {
                    printfRed("[allocate_vma_page] failed to get shared file size for %s\n",
                              vm->vfile->_path_name.c_str());
                    return size_result;
                }

                uint64 file_offset = static_cast<uint64>(vm->offset) + (page_va - vm->addr);
                if (file_offset >= st.size)
                {
                    proc::Pcb *p = proc::k_pm.get_cur_pcb();
                    proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
                    return 0;
                }
            }
            if (page_offset >= seg.real_size)
            {
                if (vm->vfile != nullptr)
                {
                    proc::Pcb *p = proc::k_pm.get_cur_pcb();
                    proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
                    return 0;
                }
                printfRed("[allocate_vma_page] shared page offset out of range: shmid=%d va=%p offset=%p real_size=%p\n",
                          vm->backing_shmid, (void *)va, (void *)page_offset, (void *)seg.real_size);
                return -1;
            }

            uint64 shared_pa = seg.phy_addrs + page_offset;
            if (reuse_existing_mapping_if_ready())
            {
                return 0;
            }
            if (!this->map_pages(pt, page_va, PGSIZE, shared_pa, pte_flags))
            {
                printfRed("[allocate_vma_page] map shared page failed: shmid=%d va=%p pa=%p\n",
                          vm->backing_shmid, (void *)page_va, (void *)shared_pa);
                return -1;
            }
            return 0;
        }

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
        fs::file *vf = vm->vfile;
        if (vf != nullptr && vm->vfd != -1)
        {
            // 文件映射：需要检查是否访问超出文件大小的区域
            int offset = vm->offset + (page_va - vm->addr);

            // 获取文件实际大小
            fs::Kstat st;
            int size_result = vfs_fstat(vf, &st);
            uint64 file_size = st.size;
            if (size_result != EOK)
            {
                printfRed("[allocate_vma_page] failed to get file size for %s\n", vf->_path_name.c_str());
                k_pmm.free_page(pa);
                return size_result;
            }
            // 检查访问是否超出文件大小
            if (offset >= (int)file_size)
            {
                printfRed("[allocate_vma_page] access beyond file size: offset=%d, file_size=%lu for %s\n",
                          offset, file_size, vf->_path_name.c_str());
                k_pmm.free_page(pa);
                // 访问超出文件大小，应该产生SIGBUS信号
                proc::Pcb *p = proc::k_pm.get_cur_pcb();
                proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
                return 0; // 返回0表示已处理信号，不再继续分配页面
            }

            // 从文件读取数据
            printfCyan("[allocate_vma_page] reading from file %s at offset %d (file_size=%lu)\n",
                       vf->_path_name.c_str(), offset, file_size);

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

        // 在本线程分配/读盘期间，另一个线程可能已经把同一页补好了。
        // 这时直接复用现有映射，并回收掉本次多余分配的物理页。
        if (reuse_existing_mapping_if_ready())
        {
            k_pmm.free_page(pa);
            return 0;
        }

        // 添加页面映射
        if (!this->map_pages(pt, page_va, PGSIZE, (uint64)pa, pte_flags))
        {
            printfRed("[allocate_vma_page] map_pages failed\n");
            k_pmm.free_page(pa);
            return -1;
        }

#ifdef LOONGARCH
        // LoongArch 的 TLB 可能保留着这页先前 fault 下来的无效表项。
        // 新页表项补好后立刻按对失效一次，避免用户态回去后还在原指令上反复 fault。
        invalidate_loongarch_user_page_pair(page_va);
#endif

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
        if (!proc || !proc->get_vma())
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
                if (proc->get_vma()->_vm[i].used)
                {
                    // 检查是否在当前VMA范围内
                    if (va >= proc->get_vma()->_vm[i].addr && va < proc->get_vma()->_vm[i].addr + proc->get_vma()->_vm[i].len)
                    {
                        target_vm = &proc->get_vma()->_vm[i];
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

            // copy_out 只能写入用户可写页；否则会把目录项等数据误写进 guard page
            // 甚至误写到被错误映射的内核页上，最终把当前进程元数据一并带坏。
            if (!pte.is_valid() || !pte.is_user() || !pte.is_writable())
            {
                printfRed("[copy_out] invalid user destination va=%p pte=%p valid=%d user=%d writable=%d pt_base=%p pid=%d tid=%d\n",
                          (void *)a,
                          (void *)pte.get_data(),
                          pte.is_valid(),
                          pte.is_user(),
                          pte.is_writable(),
                          (void *)pt.get_base(),
                          proc ? proc->_pid : -1,
                          proc ? proc->_tid : -1);
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

            if (proc != nullptr)
            {
                proc::ProcessMemoryManager *mm = proc->get_memory_manager();
                uint64 write_start = pa + (va - a);
                if ((mm != nullptr && ranges_overlap(write_start, n, (uint64)mm, sizeof(proc::ProcessMemoryManager))) ||
                    ranges_overlap(write_start, n, (uint64)proc, sizeof(proc::Pcb)) ||
                    ranges_overlap(write_start, n, (uint64)proc->get_trapframe(), sizeof(TrapFrame)) ||
                    ranges_overlap(write_start, n, pt.get_base(), PGSIZE))
                {
                    panic("[copy_out] user buffer aliases kernel object va=%p pa=%p len=%p pid=%d tid=%d mm=%p pt_base=%p trapframe=%p",
                          (void *)va,
                          (void *)write_start,
                          (void *)n,
                          proc->_pid,
                          proc->_tid,
                          mm,
                          (void *)pt.get_base(),
                          proc->get_trapframe());
                }
            }
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
        if (!proc || !proc->get_vma())
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
                if (proc->get_vma()->_vm[i].used)
                {
                    // 检查是否在当前VMA范围内
                    if (va >= proc->get_vma()->_vm[i].addr && va < proc->get_vma()->_vm[i].addr + proc->get_vma()->_vm[i].len)
                    {
                        target_vm = &proc->get_vma()->_vm[i];
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

            if (!pte.is_valid() || !pte.is_user_plv() || !pte.is_writable())
            {
                printfRed("[copy_out] invalid user destination va=%p pte=%p valid=%d user=%d writable=%d pt_base=%p pid=%d tid=%d\n",
                          (void *)a,
                          (void *)pte.get_data(),
                          pte.is_valid(),
                          pte.is_user_plv(),
                          pte.is_writable(),
                          (void *)pt.get_base(),
                          proc ? proc->_pid : -1,
                          proc ? proc->_tid : -1);
                return -1;
            }

            pa = reinterpret_cast<uint64>(pte.pa());
            if (pa == 0)
                return -1;
            n = PGSIZE - (va - a);
            if (n > len)
                n = len;
            pa = to_vir(pa);

            if (proc != nullptr)
            {
                proc::ProcessMemoryManager *mm = proc->get_memory_manager();
                uint64 write_start = pa + (va - a);
                if ((mm != nullptr && ranges_overlap(write_start, n, (uint64)mm, sizeof(proc::ProcessMemoryManager))) ||
                    ranges_overlap(write_start, n, (uint64)proc, sizeof(proc::Pcb)) ||
                    ranges_overlap(write_start, n, (uint64)proc->get_trapframe(), sizeof(TrapFrame)) ||
                    ranges_overlap(write_start, n, to_vir(pt.get_base()), PGSIZE))
                {
                    panic("[copy_out] user buffer aliases kernel object va=%p pa=%p len=%p pid=%d tid=%d mm=%p pt_base=%p trapframe=%p",
                          (void *)va,
                          (void *)write_start,
                          (void *)n,
                          proc->_pid,
                          proc->_tid,
                          mm,
                          (void *)pt.get_base(),
                          proc->get_trapframe());
                }
            }
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
#ifdef RISCV
            bool is_reserved_page = (a == TRAMPOLINE || a == SIG_TRAMPOLINE ||
                                     (a == TRAPFRAME && !(va == TRAPFRAME && npages == 1 && do_free == 0)));
#elif defined(LOONGARCH)
            bool is_reserved_page = (a == SIG_TRAMPOLINE ||
                                     (a == TRAPFRAME && !(va == TRAPFRAME && npages == 1 && do_free == 0)));
#endif
            if (is_reserved_page)
            {
                // 这些保留页由专门路径管理。
                // 统一清理流程允许调用者把它们一起带进来，但这属于正常退出场景，不应反复打印告警。
            }
            if ((pte = pt.walk(a, 0)).is_null())
                continue;
            // panic("vmunmap: walk");
            if (!pte.is_valid())
                continue;
            ///@brief 这里的逻辑是，如果pte无效，则不需要释放物理页
            /// 为了mmap的懒分配，所以确实可能出现了惰性页面调用
            // panic("vmunmap: not mapped");
            // if (!pte.is_leaf())
            //     panic("vmunmap: not a leaf");  //目前没搞懂为什么共享内存那一片free会爆这个，先关掉试试。
            if (do_free)
            {
                // printfMagenta("vmunmap: free va: %p, pa: %p\n", a, pte.pa());
                k_pmm.free_page(page_pa_to_kernel_ptr(reinterpret_cast<uint64>(pte.pa())));
            }
            // printfMagenta("vmunmap: unmap va: %p, pa: %p\n", a, pte.pa());
            pte.clear_data();
#ifdef LOONGARCH
            invalidate_loongarch_user_page_pair(a);
#endif
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

    int VirtualMemoryManager::vm_copy(PageTable &old_pt, PageTable &new_pt, uint64 start, uint64 size)
    {
        Pte pte;
        uint64 pa, va;
        uint64 va_end;
        uint64 flags;
        void *mem;

        if (size == 0)
        {
            return 0;
        }

        uint64 copy_start = PGROUNDDOWN(start);
        va_end = PGROUNDUP(start + size);
        if (va_end < copy_start)
        {
            printfRed("uvmcopy: address range overflow, start=%p size=%p\n",
                      (void *)start, (void *)size);
            return -1;
        }
        if (copy_start != start || PGROUNDUP(size) != size)
        {
            printfYellow("uvmcopy: 自动对齐复制范围 start=%p size=%p -> [%p, %p)\n",
                         (void *)start, (void *)size, (void *)copy_start, (void *)va_end);
        }

        for (va = copy_start; va < va_end; va += PGSIZE)
        {
            if ((pte = old_pt.walk(va, false)).is_null())
            {
                continue;
            }
            if (pte.is_valid() == 0)
                continue;
            ///@brief 这里的逻辑是，如果pte无效，则不需要释放物理页
            /// TODO: 为了mmap的懒分配，所以确实可能出现了惰性页面调用
            // panic("uvmcopy: page not valid");
            pa = (uint64)pte.pa();
            flags = pte.get_flags();

            // 检查当前虚拟地址是否属于共享内存区域
            void *shm_start_addr = nullptr;
            size_t shm_size = 0;
            int is_shared = shm::k_smm.find_shared_memory_segment((void *)va, &shm_start_addr, &shm_size);

            if (is_shared >= 0)
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
                memmove(mem, page_pa_to_kernel_ptr(pa), PGSIZE);
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
            // 统一补齐 MAT，避免调用方漏传时只有 LA 的 exec/brk 数据页不可缓存。
            uint64 pte_flags = flags | PTE_U | PTE_D | PTE_MAT;
            if (map_pages(pt, a, PGSIZE, (uint64)mem, pte_flags) == 0)
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
        kvmmap(pt, VIRTIO_NET_MMIO_BASE, VIRTIO_NET_MMIO_BASE, PGSIZE, PTE_R | PTE_W);
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
        uint64 phys_top = k_pmm.get_phys_top();
        if (phys_top <= (uint64)etext)
        {
            panic("[vmm] invalid phys_top %p vs etext %p", phys_top, etext);
        }
        kvmmap(pt, (uint64)etext, (uint64)etext, phys_top - (uint64)etext, PTE_R | PTE_W);
        // printfRed("[vmm] kvmmake kernel data success\n");

        // Map DTB if it exists
        if (k_dtb_addr != 0) {
             // Map a reasonable size for DTB (e.g. 2MB to be safe and cover crossing page boundaries)
             // We map to identity address
             kvmmap(pt, k_dtb_addr, k_dtb_addr, 0x200000, PTE_R | PTE_W);
             // printfGreen("[vmm] mapped DTB at %p\n", k_dtb_addr);
        }

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
        kvmmap(pt, vm_kernel_heap_start, mem::k_pmm.get_heap_area_start(), mem::k_pmm.get_heap_area_size(), PTE_R | PTE_W);
#elif defined(LOONGARCH)
        // LoongArch virt 既可能是单段低端 RAM，也可能像 QEMU 1G 那样拆成低/高两段。
        // 因此这里不再假设 “etext 到 PHYSTOP” 是一整段连续内存：
        // 1. 先映射包含内核镜像的低端连续区；
        // 2. 再按需单独映射高端的 heap/shm 区。
        uint64 low_map_top = mem::k_pmm.get_kernel_linear_top();
        if (low_map_top <= (uint64)etext)
        {
            panic("[vmm] invalid low_map_top %p vs etext %p", low_map_top, etext);
        }
        kvmmap(pt, ((uint64)etext) & (~(DMWIN_MASK)), (uint64)etext, low_map_top - (uint64)etext, PTE_R | PTE_W);

        uint64 heap_start = mem::k_pmm.get_heap_area_start();
        uint64 heap_size = mem::k_pmm.get_heap_area_size();
        uint64 low_map_start_va = (uint64)etext;
        if (!(heap_start >= low_map_start_va && heap_start + heap_size <= low_map_top))
        {
            kvmmap(pt, heap_start & (~(DMWIN_MASK)), heap_start, heap_size, PTE_R | PTE_W);
        }
        
        // Map DTB if it exists
        if (k_dtb_addr != 0) {
            uint64 dtb_size = PGROUNDUP(0x10000); // Assume 64KB for DTB
            uint64 dtb_va = k_dtb_addr & (~(DMWIN_MASK));
            kvmmap(pt, dtb_va, k_dtb_addr, dtb_size, PTE_R | PTE_W);
            printfGreen("[vmm] Mapped DTB at va=%p pa=%p, size=%p\n", dtb_va, k_dtb_addr, dtb_size);
        }

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

    int VirtualMemoryManager::protectpages(PageTable &pt, uint64 va, uint64 size, int prot, bool is_vma)
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
                uint64 old_data = pte.get_data();
#ifdef RISCV
                // RISC-V 直接以 R/W/X 三个位表达权限。
                uint64 new_data = old_data &
                                  ~(riscv::PteEnum::pte_readable_m |
                                    riscv::PteEnum::pte_writable_m |
                                    riscv::PteEnum::pte_executable_m);
                if (prot & PROT_READ)
                    new_data |= riscv::PteEnum::pte_readable_m;
                if (prot & PROT_WRITE)
                    new_data |= riscv::PteEnum::pte_writable_m;
                if (prot & PROT_EXEC)
                    new_data |= riscv::PteEnum::pte_executable_m;
                new_data |= riscv::PteEnum::pte_valid_m | riscv::PteEnum::pte_user_m;
#elif defined(LOONGARCH)
                // LoongArch 的读/执行权限是“禁止位”(NR/NX)，而不是正向的 R/X 位。
                // mprotect(PROT_NONE) / 取消执行权限都必须显式写回 NR/NX。
                uint64 new_data = old_data & ~(PTE_W | PTE_NR | PTE_NX | PTE_PLV);
                new_data |= PTE_V | PTE_U;
                if (prot & PROT_WRITE)
                    new_data |= PTE_W;
                if (!(prot & PROT_READ))
                    new_data |= PTE_NR;
                if (!(prot & PROT_EXEC))
                    new_data |= PTE_NX;
#endif
                pte.set_data(new_data);
            }
            else
            {
#ifdef RISCV
                pte.set_data(pte.get_data() | riscv::PteEnum::pte_user_m);
#elif defined(LOONGARCH)
                pte.set_data(pte.get_data() | PTE_U);
#endif
            }
        }
        return 0;
    }
}

#include "klib.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "kernel_image.hh"
#include "mem.hh" // 添加mmap相关常量定义
#ifdef RISCV
#include "hal/riscv/platform_board.hh"
#include "mem/riscv/pagetable.hh"
#elif defined(LOONGARCH)
#include "mem/loongarch/pagetable.hh"
#endif
#include "memlayout.hh"
#include "libs/algorithm.hh"
#include "printer.hh"
#include "fs/vfs/vfs_ext4_ext.hh" // 添加vfs_ext_get_filesize函数
#include "proc/signal.hh"         // 添加信号处理
#include "proc/process_memory_manager.hh"
#include "proc/vm_object.hh"
#include "proc/proc_manager.hh"   // 添加进程管理
#include "fs/lwext4/ext4_errno.hh"
#include "proc/proc.hh"
#include "proc_manager.hh"
#include "sys/syscall_defs.hh"
#include "fs/vfs/vfs_utils.hh"
#include "fs/vfs/virtual_fs.hh"
#include "hal/tlb_shootdown.hh"
#include "devs/dtb.hh"
#include "libs/perf_diag.hh"
extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S
extern uint64 k_dtb_addr; // Defined in main.cc
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;

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
        void flush_user_pt_range(PageTable &pt, uint64 start, uint64 size);

        inline void *page_pa_to_kernel_ptr(uint64 pa)
        {
#ifdef LOONGARCH
            return reinterpret_cast<void *>(to_vir(pa));
#else
            return reinterpret_cast<void *>(pa);
#endif
        }

#ifdef RISCV
        inline bool pte_is_cow(Pte &pte)
        {
            return !pte.is_null() &&
                   pte.is_valid() &&
                   ((pte.get_data() & k_riscv_pte_cow) != 0);
        }

        inline void flush_riscv_user_page(uint64 va)
        {
            proc::Pcb *current = proc::k_pm.get_cur_pcb();
            if (current != nullptr && current->get_memory_manager() != nullptr)
            {
                flush_user_pt_range(
                    *current->get_pagetable(), PGROUNDDOWN(va), PGSIZE);
            }
            else
            {
                hal::tlb::flush_range_all_cpus(PGROUNDDOWN(va), PGSIZE);
            }
        }
#elif defined(LOONGARCH)
        inline bool pte_is_cow(Pte &pte)
        {
            return !pte.is_null() &&
                   pte.is_valid() &&
                   ((pte.get_data() & PTE_COW) != 0);
        }
#endif

        proc::Pcb *active_proc_for_pt(PageTable &pt)
        {
            proc::Pcb *proc = proc::k_pm.get_cur_pcb();
            if (proc == nullptr || proc->get_pagetable() == nullptr)
            {
                return nullptr;
            }
            if (proc->get_pagetable()->get_base() != pt.get_base())
            {
                return nullptr;
            }
            return proc;
        }

        proc::ProcessMemoryManager *resolve_target_mm(PageTable &pt,
                                                      proc::ProcessMemoryManager *target_mm)
        {
            // 用户页表只是硬件地址转换结构；懒分配、栈增长和 COW 拆页还需要同一地址空间的
            // VMASpace 元数据。调用者显式给出 target_mm 时，以该 mm 作为唯一语义归属。
            if (target_mm != nullptr)
            {
                return target_mm;
            }

            // 没有显式 target_mm 时，只能在页表确实属于当前运行进程时推导 mm。
            // 这样 copy_in/copy_out 的缺页处理始终作用在页表所属的地址空间内。
            proc::Pcb *proc = active_proc_for_pt(pt);
            return proc != nullptr ? proc->get_memory_manager() : nullptr;
        }

        void flush_user_pt_range(PageTable &pt, uint64 start, uint64 size)
        {
            proc::ProcessMemoryManager *mm = resolve_target_mm(pt, nullptr);
            if (mm != nullptr)
            {
                hal::tlb::flush_mm_range(*mm, start, size);
            }
            else
            {
                // detached child 页表/内核初始化不属于任何活跃 mm，只能保留
                // 全局后端作为生命周期边界；BuildStorm 热路径会命中上面的 mm。
                hal::tlb::flush_range_all_cpus(start, size);
            }
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

        proc::vma *active_vma_for_page(PageTable &pt, uint64 va)
        {
            proc::Pcb *proc = active_proc_for_pt(pt);
            if (proc == nullptr || proc->get_memory_manager() == nullptr)
            {
                return nullptr;
            }
            return proc->get_memory_manager()->find_vma_covering(PGROUNDDOWN(va));
        }

        bool vma_has_shared_write_semantics(const proc::vma *vm)
        {
            if (vm == nullptr)
            {
                return false;
            }
            if (vm->object != nullptr && vm->object->shared_mapping())
            {
                return true;
            }
            return vm->is_shared_mapping();
        }

        bool vma_has_private_cow_semantics(const proc::vma *vm)
        {
            if (vm == nullptr)
            {
                return false;
            }
            if (vma_has_shared_write_semantics(vm))
            {
                return false;
            }
            return vm->is_private_mapping() ||
                   (vm->object != nullptr && !vm->object->shared_mapping());
        }

        bool should_preserve_cow_without_write(PageTable &pt, uint64 va, Pte &pte, bool is_vma)
        {
            if (!is_vma || !pte_is_cow(pte))
            {
                return false;
            }
            return vma_has_private_cow_semantics(active_vma_for_page(pt, va));
        }

        bool should_write_protect_as_cow(PageTable &pt, uint64 va, Pte &pte, bool is_vma)
        {
            void *page = page_pa_to_kernel_ptr(reinterpret_cast<uint64>(pte.pa()));
            if (!k_pmm.is_managed_page(page))
            {
                return false;
            }

            if (!is_vma)
            {
                return k_pmm.page_ref_count(page) > 1;
            }

            proc::vma *vm = active_vma_for_page(pt, va);
            if (vma_has_shared_write_semantics(vm))
            {
                return false;
            }
            if (vma_has_private_cow_semantics(vm))
            {
                return pte_is_cow(pte) || k_pmm.page_ref_count(page) > 1;
            }
            return k_pmm.page_ref_count(page) > 1;
        }

#ifdef LOONGARCH
        uint64 loongarch_empty_pgdh_base = 0;

        inline void invalidate_loongarch_user_page_pair(uint64 va)
        {
            proc::Pcb *current = proc::k_pm.get_cur_pcb();
            if (current != nullptr && current->get_memory_manager() != nullptr)
            {
                flush_user_pt_range(*current->get_pagetable(),
                                    va & ~((PGSIZE << 1) - 1), PGSIZE << 1);
            }
            else
            {
                hal::tlb::flush_range_all_cpus(
                    va & ~((PGSIZE << 1) - 1), PGSIZE << 1);
            }
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

        int resolve_user_read_pa(PageTable &pt,
                                 proc::ProcessMemoryManager *target_mm,
                                 proc::Pcb *proc,
                                 uint64 user_va,
                                 uint64 &out_pa)
        {
            uint64 page_va = PGROUNDDOWN(user_va);
            Pte pte = pt.walk(page_va, false);

            if (pte.is_null() || pte.get_data() == 0)
            {
                if (target_mm == nullptr || target_mm->fault_page(user_va, 0) != 0)
                {
                    printfRed("[resolve_user_read_pa] walk failed for va: %p\n", user_va);
                    return -1;
                }
                pte = pt.walk(page_va, false);
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

        int resolve_user_write_pa(PageTable &pt,
                                  proc::ProcessMemoryManager *target_mm,
                                  proc::Pcb *proc,
                                  uint64 user_va,
                                  uint64 &out_pa)
        {
            uint64 page_va = PGROUNDDOWN(user_va);
            Pte pte = pt.walk(page_va, false);
            if (pte.is_null() || pte.get_data() == 0)
            {
                if (target_mm == nullptr || target_mm->fault_page(user_va, 1) != 0)
                {
                    return -1;
                }
                pte = pt.walk(page_va, false);
            }

#ifdef RISCV
            bool user_page = !pte.is_null() && pte.is_user();
#elif defined(LOONGARCH)
            bool user_page = !pte.is_null() && pte.is_user_plv();
#endif
            if (!pte.is_null() && pte.is_valid() && user_page &&
                !pte.is_writable() && pte_is_cow(pte))
            {
                if (target_mm == nullptr || target_mm->fault_page(user_va, 1) != 0)
                {
                    return -1;
                }
                pte = pt.walk(page_va, false);
#ifdef RISCV
                user_page = !pte.is_null() && pte.is_user();
#elif defined(LOONGARCH)
                user_page = !pte.is_null() && pte.is_user_plv();
#endif
            }

            if (pte.is_null() || !pte.is_valid() || !user_page || !pte.is_writable())
            {
                return -1;
            }
            out_pa = reinterpret_cast<uint64>(pte.pa());
            if (out_pa == 0)
            {
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
        for (proc::Pcb &pcb : proc::k_proc_pool)
        {
            pcb.map_kstack(k_pagetable);
        }

        activate_kernel_pagetable();

        printfGreen("[vmm] Virtual Memory Manager Init\n");
    }

    void VirtualMemoryManager::activate_kernel_pagetable()
    {
#ifdef RISCV
        // satp 是每个 hart 的寄存器。主核建表后和次核上线时都要各自刷新
        // 本地 TLB，不能把“写过一次 satp”误当成全局状态。
        sfence_vma();
        w_satp(riscv::make_satp(k_pagetable.get_base()));
        sfence_vma();
#elif defined(LOONGARCH)
        // PGDL/PGDH、页表遍历参数和 TLB 同样属于每个 CPU。空 PGDH 已由
        // 主核在 init() 中分配；次核此处只复用它，不再并发分配页表页。
        w_csr_pgdl((uint64)k_pagetable.get_base());
        install_loongarch_empty_high_user_pagetable();
        tlbinit();

        w_csr_pwcl((PTEWIDTH << 30) | (DIR2WIDTH << 25) | (DIR2BASE << 20) | (DIR1WIDTH << 15) | (DIR1BASE << 10) | (PTWIDTH << 5) | (PTBASE << 0));
        w_csr_pwch((DIR4WIDTH << 18) | (DIR3WIDTH << 6) | (DIR3BASE << 0) | (PWCH_HPTW_EN << 24));
#endif
    }

    void VirtualMemoryManager::lock_page_table_updates()
    {
        _virt_mem_lock.acquire();
    }

    void VirtualMemoryManager::unlock_page_table_updates()
    {
        _virt_mem_lock.release();
    }

    // 根据传入的 flags 标志，生成对应的页表权限（perm）值
    bool VirtualMemoryManager::map_pages(PageTable &pt, uint64 va, uint64 size, uint64 pa, uint64 flags)
    {
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
            {
                bool is_kernel_pt = !k_pagetable.is_null() && pt.get_base() == k_pagetable.get_base();
                bool is_user_va = a < USER_MEMORY_TOP;
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
            // RISC-V 允许硬件在 A/D 位为零时直接产生页故障，而不是自动置位。
            // 内核映射没有用户缺页修复路径，因此安装叶子 PTE 时预置两位；
            // 实际读写权限仍只由调用方传入的 R/W/X 标志决定。
            pte.set_data(PA2PTE(PGROUNDDOWN(riscv::virt_to_phy_address(pa))) |
                         flags |
                         riscv::PteEnum::pte_valid_m |
                         riscv::PteEnum::pte_accessed_m |
                         riscv::PteEnum::pte_dirty_m);
#elif defined(LOONGARCH)
            pte.set_data(PA2PTE(PGROUNDDOWN(pa)) |
                         flags |
                         loongarch::pte_valid_m);
#endif
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
            mem = PhysicalMemoryManager::try_alloc_page();
            if (mem == nullptr)
            {
                vmdealloc(pt, a, old_sz);
                return 0;
            }
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
            mem = PhysicalMemoryManager::try_alloc_page();
            if (mem == nullptr)
            {
                printfRed("vmalloc: alloc_page failed\n");
                vmdealloc(pt, a, old_sz);
                return 0;
            }
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
    int VirtualMemoryManager::copy_in(PageTable &pt,
                                      void *dst,
                                      uint64 src_va,
                                      uint64 len,
                                      proc::ProcessMemoryManager *target_mm)
    {
        uint64 n, va, pa;
        char *p_dst = (char *)dst;
        proc::Pcb *proc = active_proc_for_pt(pt);
        target_mm = resolve_target_mm(pt, target_mm);
        if (target_mm == nullptr)
        {
            return -1;
        }

        while (len > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, target_mm, proc, src_va, pa) != 0)
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

    int VirtualMemoryManager::ensure_user_read_range(PageTable &pt, uint64 src_va, uint64 len)
    {
        if (len == 0)
        {
            return 0;
        }
        if (src_va + len < src_va)
        {
            return -1;
        }

        proc::Pcb *proc = active_proc_for_pt(pt);
        proc::ProcessMemoryManager *target_mm = resolve_target_mm(pt, nullptr);
        if (target_mm == nullptr)
        {
            return -1;
        }
        uint64 cursor = src_va;
        uint64 remaining = len;
        while (remaining > 0)
        {
            uint64 page_va = PGROUNDDOWN(cursor);
            uint64 ignored_pa = 0;
            if (resolve_user_read_pa(pt, target_mm, proc, cursor, ignored_pa) != 0)
            {
                return -1;
            }

            uint64 chunk = PGSIZE - (cursor - page_va);
            if (chunk > remaining)
            {
                chunk = remaining;
            }
            cursor += chunk;
            remaining -= chunk;
        }
        return 0;
    }

    int VirtualMemoryManager::ensure_user_write_range(PageTable &pt, uint64 dst_va, uint64 len)
    {
        if (len == 0)
        {
            return 0;
        }
        if (dst_va + len < dst_va)
        {
            return -1;
        }

        proc::Pcb *proc = active_proc_for_pt(pt);
        proc::ProcessMemoryManager *target_mm = resolve_target_mm(pt, nullptr);
        if (target_mm == nullptr)
        {
            return -1;
        }
        uint64 cursor = dst_va;
        uint64 remaining = len;
        while (remaining > 0)
        {
            uint64 page_va = PGROUNDDOWN(cursor);
            uint64 ignored_pa = 0;
            if (resolve_user_write_pa(pt, target_mm, proc, cursor, ignored_pa) != 0)
            {
                return -1;
            }

            uint64 chunk = PGSIZE - (cursor - page_va);
            if (chunk > remaining)
            {
                chunk = remaining;
            }
            cursor += chunk;
            remaining -= chunk;
        }
        return 0;
    }

    int VirtualMemoryManager::user_read_kernel_address(PageTable &pt, uint64 src_va, uint64 &kernel_addr)
    {
        proc::Pcb *proc = active_proc_for_pt(pt);
        proc::ProcessMemoryManager *target_mm = resolve_target_mm(pt, nullptr);
        uint64 page_addr = 0;
        if (target_mm == nullptr ||
            resolve_user_read_pa(pt, target_mm, proc, src_va, page_addr) != 0)
        {
            return -1;
        }
        kernel_addr = page_addr + (src_va - PGROUNDDOWN(src_va));
        return 0;
    }

    int VirtualMemoryManager::copy_str_in(PageTable &pt, void *dst,
                                          uint64 src_va, uint64 max)
    {
        uint64 n, va, pa;
        int got_null = 0;
        char *p_dst = (char *)dst;
        proc::Pcb *proc = active_proc_for_pt(pt);
        proc::ProcessMemoryManager *target_mm = resolve_target_mm(pt, nullptr);
        if (target_mm == nullptr)
        {
            return -1;
        }

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, target_mm, proc, src_va, pa) != 0)
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
        proc::Pcb *proc = active_proc_for_pt(pt);
        proc::ProcessMemoryManager *target_mm = resolve_target_mm(pt, nullptr);
        if (target_mm == nullptr)
        {
            return -EFAULT;
        }

        while (got_null == 0 && max > 0)
        {
            va = PGROUNDDOWN(src_va);
            if (resolve_user_read_pa(pt, target_mm, proc, src_va, pa) != 0)
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
        F7LY_PERF_ADD(PageFault, 1);
        F7LY_PERF_SCOPE(PageFaultTimeTicks);
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
            vm->has_resident_pages = true;
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
        if (vm->object != nullptr)
        {
            proc::VmPageView view = {};
            int prepare_result = vm->object->prepare_page(*vm, vm->page_index_for_va(page_va), access_type, view);
            if (prepare_result != 0)
            {
                printfRed("[allocate_vma_page] vm object prepare failed va=%p object=%p ret=%d\n",
                          (void *)page_va, vm->object, prepare_result);
                return -1;
            }
            if (view.signal_delivered)
            {
                return 0;
            }
            if (view.pa == 0)
            {
                printfRed("[allocate_vma_page] vm object returned empty page va=%p object=%p\n",
                          (void *)page_va, vm->object);
                return -1;
            }

            uint64 object_pte_flags = pte_flags;
#ifdef RISCV
            if (view.mark_cow)
            {
                object_pte_flags &= ~riscv::PteEnum::pte_writable_m;
                object_pte_flags |= k_riscv_pte_cow;
            }
#elif defined(LOONGARCH)
            if (view.mark_cow)
            {
                object_pte_flags &= ~(PTE_W | PTE_D);
                object_pte_flags |= PTE_COW;
            }
#endif
            if (view.writable)
            {
#ifdef RISCV
                object_pte_flags |= riscv::PteEnum::pte_writable_m | riscv::PteEnum::pte_readable_m;
#elif defined(LOONGARCH)
                object_pte_flags |= PTE_W | PTE_D;
#endif
            }

            if (reuse_existing_mapping_if_ready())
            {
                k_pmm.free_page(page_pa_to_kernel_ptr(view.pa));
                vm->has_resident_pages = true;
                return 0;
            }

            if (!this->map_pages(pt, page_va, PGSIZE, view.pa, object_pte_flags))
            {
                k_pmm.free_page(page_pa_to_kernel_ptr(view.pa));
                printfRed("[allocate_vma_page] map vm object page failed va=%p pa=%p flags=0x%x\n",
                          (void *)page_va, (void *)view.pa, (uint32)object_pte_flags);
                return -1;
            }
            vm->has_resident_pages = true;
#ifdef LOONGARCH
            invalidate_loongarch_user_page_pair(page_va);
#endif
            return 0;
        }

        fs::file *vf = vm->vfile;
        // ELF/动态库读取存在跨层短读路径；在所有读入分支都能证明完整覆盖前，
        // 文件映射仍从零页开始，避免未初始化字节进入动态链接器元数据。
        void *pa = k_pmm.try_alloc_page();
        if (pa == nullptr)
        {
            printfRed("[allocate_vma_page] alloc_page failed for va: %p\n", va);
            return -1;
        }

        // 检查是否为文件映射
        if (vf != nullptr && vm->vfd != -1)
        {
            // 文件映射：需要检查是否访问超出文件大小的区域
            const uint64 page_delta = page_va - vm->addr;
            if (vm->offset > UINT64_MAX - page_delta)
            {
                printfRed("[allocate_vma_page] file offset overflow: base=%lu delta=%lu\n",
                          vm->offset, page_delta);
                k_pmm.free_page(pa);
                return -1;
            }
            const uint64 offset = vm->offset + page_delta;

            // 获取文件实际大小
            fs::Kstat st;
            int size_result = fs::k_vfs.fstat(vf, &st);
            uint64 file_size = st.size;
            if (size_result != EOK)
            {
                printfRed("[allocate_vma_page] failed to get file size for %s\n", vf->_path_name.c_str());
                k_pmm.free_page(pa);
                return size_result;
            }
            // 检查访问是否超出文件大小
            if (offset >= file_size)
            {
                printfRed("[allocate_vma_page] access beyond file size: offset=%lu, file_size=%lu for %s\n",
                          offset, file_size, vf->_path_name.c_str());
                k_pmm.free_page(pa);
                // 访问超出文件大小，应该产生SIGBUS信号
                proc::Pcb *p = proc::k_pm.get_cur_pcb();
                proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
                return 0; // 返回0表示已处理信号，不再继续分配页面
            }

            // 从文件读取数据
            if (offset > static_cast<uint64>(LONG_MAX))
            {
                k_pmm.free_page(pa);
                return -1;
            }
            int readbytes = vf->read((uint64)pa, PGSIZE,
                                     static_cast<long>(offset), false);
            if (readbytes < 0)
            {
                printfRed("[allocate_vma_page] file read failed\n");
                k_pmm.free_page(pa);
                return -1;
            }

            if (readbytes < PGSIZE)
            {
                // 文件短页必须按 mmap 语义补零；完整文件页不需要预先清零，
                // 否则 lmbench 的 pagefault/mmap 会在每次缺页上多刷一遍 4K。
                memset((char *)pa + readbytes, 0, PGSIZE - readbytes);
            }
        }

        // 在本线程分配/读盘期间，另一个线程可能已经把同一页补好了。
        // 这时直接复用现有映射，并回收掉本次多余分配的物理页。
        if (reuse_existing_mapping_if_ready())
        {
            vm->has_resident_pages = true;
            k_pmm.free_page(pa);
            return 0;
        }

        // 对象映射的私有页由 VmObject::prepare_page() 自己维护 overlay；
        // 这里处理的是普通匿名私有映射，页表已经是驻留页权威，munmap/退出时扫描页表即可。
        // 若每次匿名缺页都插入 unordered_map，musl malloc 的 4K 压力会被放大到秒级。
        const bool track_private_resident_page = false;
        const uint64 private_page_index =
            track_private_resident_page ? vm->page_index_for_va(page_va) : 0;
        bool private_overlay_created = false;
        bool private_overlay_retained = false;

        if (track_private_resident_page)
        {
            // 私有非对象映射如果不单独记住已驻留页，线程栈和大块匿名区在退出/munmap 时
            // 只能按整段 VMA 扫页。overlay 保存“页号 -> 物理页”索引，并额外持有一份
            // 元数据 owner 引用；页表映射释放一份，VMA 元数据销毁再释放这份 owner 引用。
            if (vm->private_page_overlay == nullptr)
            {
                vm->private_page_overlay = new proc::VmPrivateOverlayMap();
                if (vm->private_page_overlay == nullptr)
                {
                    k_pmm.free_page(pa);
                    return -1;
                }
                private_overlay_created = true;
            }

            if (!k_pmm.retain_page(page_pa_to_kernel_ptr(reinterpret_cast<uint64>(pa))))
            {
                if (private_overlay_created && vm->private_page_overlay != nullptr &&
                    vm->private_page_overlay->empty())
                {
                    delete vm->private_page_overlay;
                    vm->private_page_overlay = nullptr;
                }
                k_pmm.free_page(pa);
                return -1;
            }

            private_overlay_retained = true;
            (*vm->private_page_overlay)[private_page_index] = reinterpret_cast<uint64>(pa);
        }

        // 添加页面映射
        if (!this->map_pages(pt, page_va, PGSIZE, (uint64)pa, pte_flags))
        {
            if (private_overlay_retained && vm->private_page_overlay != nullptr)
            {
                vm->private_page_overlay->erase(private_page_index);
                if (private_overlay_created && vm->private_page_overlay->empty())
                {
                    delete vm->private_page_overlay;
                    vm->private_page_overlay = nullptr;
                }
                k_pmm.free_page(pa);
            }
            printfRed("[allocate_vma_page] map_pages failed\n");
            k_pmm.free_page(pa);
            return -1;
        }
        vm->has_resident_pages = true;

#ifdef LOONGARCH
        // LoongArch 的 TLB 可能保留着这页先前 fault 下来的无效表项。
        // 新页表项补好后立刻按对失效一次，避免用户态回去后还在原指令上反复 fault。
        invalidate_loongarch_user_page_pair(page_va);
#endif

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
    int VirtualMemoryManager::copy_out(PageTable &pt,
                                       uint64 va,
                                       const void *p,
                                       uint64 len,
                                       proc::ProcessMemoryManager *target_mm)
    {
#ifdef RISCV
        uint64 n, a, pa;
        proc::Pcb *proc = active_proc_for_pt(pt);
        target_mm = resolve_target_mm(pt, target_mm);

        if (target_mm == nullptr)
        {
            printfRed("[copy_out] target mm not present, skip copy\n");
            return -1;
        }

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            Pte pte = pt.walk(a, 0);
            if (pte.is_null() || pte.get_data() == 0)
            {
                if (target_mm->fault_page(va, 1) != 0)
                {
                    printfRed("[copy_out] walk failed for va: %p\n", va);
                    return -1;
                }
                pte = pt.walk(a, 0);
            }

            // copy_out 只能写入用户可写页；否则会把目录项等数据误写进 guard page
            // 甚至误写到被错误映射的内核页上，最终把当前进程元数据一并带坏。
            if (pte.is_valid() && pte.is_user() && !pte.is_writable() && pte_is_cow(pte))
            {
                if (target_mm->fault_page(a, 1) != 0)
                {
                    return -1;
                }
                pte = pt.walk(a, 0);
            }

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

            memmove((void *)(pa + (va - a)), p, n);

            len -= n;
            p = (char *)p + n;
            va = a + PGSIZE;
        }
        return 0;
#elif defined(LOONGARCH)
        uint64 n, a, pa;
        proc::Pcb *proc = active_proc_for_pt(pt);
        target_mm = resolve_target_mm(pt, target_mm);

        if (target_mm == nullptr)
        {
            printfRed("[copy_out] target mm not present, skip copy\n");
            return -1;
        }

        while (len > 0)
        {
            a = PGROUNDDOWN(va);
            Pte pte = pt.walk(a, 0);
            if (pte.is_null() || pte.get_data() == 0)
            {
                if (target_mm->fault_page(va, 1) != 0)
                {
                    printfRed("[copy_out] walk failed for va: %p (not in any VMA)\n", va);
                    return -1;
                }
                pte = pt.walk(a, 0);
            }

            // fork 后的私有页在 LoongArch 上同样可能是 COW 只读页。
            // 内核 copy_out 写用户缓冲时需要先拆页，否则会把合法写入误判成权限错误。
            if (pte.is_valid() && pte.is_user_plv() && !pte.is_writable() && pte_is_cow(pte))
            {
                if (target_mm->fault_page(a, 1) != 0)
                {
                    return -1;
                }
                pte = pt.walk(a, 0);
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

            memmove((void *)((pa + (va - a))), p, n);

            len -= n;
            p = (char *)p + n;
            va = a + PGSIZE;
        }
        return 0;
#endif
    }

    int VirtualMemoryManager::resolve_cow_page(PageTable &pt,
                                               uint64 va,
                                               proc::ProcessMemoryManager *target_mm)
    {
        uint64 page_va = PGROUNDDOWN(va);
        Pte pte = pt.walk(page_va, false);
        proc::vma *cow_vm = nullptr;
        uint64 cow_page_index = 0;
        bool object_private_mapping = false;
        bool overlay_owned_by_area = false;

        target_mm = resolve_target_mm(pt, target_mm);
        if (target_mm != nullptr)
        {
            cow_vm = target_mm->find_vma_covering(page_va);
            if (cow_vm != nullptr &&
                cow_vm->object != nullptr &&
                cow_vm->is_private_mapping())
            {
                object_private_mapping = true;
                cow_page_index = cow_vm->page_index_for_va(page_va);
            }
        }
#ifdef RISCV
        if (pte.is_null() || !pte.is_valid() || !pte.is_user() || !pte_is_cow(pte))
        {
            return -1;
        }

        uint64 old_pa = reinterpret_cast<uint64>(pte.pa());
        void *old_page = page_pa_to_kernel_ptr(old_pa);
        if (!k_pmm.is_managed_page(old_page))
        {
            return -1;
        }

        uint64 new_flags = (pte.get_flags() | riscv::PteEnum::pte_writable_m) & ~k_riscv_pte_cow;
        uint16 refcount = k_pmm.page_ref_count(old_page);
        if (refcount == 0)
        {
            return -1;
        }

        if (object_private_mapping &&
            cow_vm->private_page_overlay != nullptr)
        {
            auto overlay = cow_vm->private_page_overlay->find(cow_page_index);
            overlay_owned_by_area = overlay != cow_vm->private_page_overlay->end() &&
                                    overlay->second == old_pa;
        }

        if ((!object_private_mapping && refcount == 1) ||
            (object_private_mapping && overlay_owned_by_area && refcount <= 2))
        {
            pte.set_data(PA2PTE(PGROUNDDOWN(old_pa)) |
                         new_flags |
                         riscv::PteEnum::pte_valid_m);
            flush_riscv_user_page(page_va);
            return 0;
        }

        void *new_page = k_pmm.try_alloc_page_uninitialized();
        if (new_page == nullptr)
        {
            return -1;
        }
        memmove(new_page, old_page, PGSIZE);

        if (object_private_mapping)
        {
            if (cow_vm->private_page_overlay == nullptr)
            {
                cow_vm->private_page_overlay = new proc::VmPrivateOverlayMap();
                if (cow_vm->private_page_overlay == nullptr)
                {
                    k_pmm.free_page(new_page);
                    return -1;
                }
            }

            if (!k_pmm.retain_page(new_page))
            {
                k_pmm.free_page(new_page);
                return -1;
            }

            if (overlay_owned_by_area)
            {
                // 当前 VMA 原本就持有这张 overlay 页的 owner 引用；
                // COW 之后条目改指向新页，旧页上的 owner 引用也要一并放掉。
                k_pmm.free_page(old_page);
            }
            (*cow_vm->private_page_overlay)[cow_page_index] =
                riscv::virt_to_phy_address(reinterpret_cast<uint64>(new_page));
        }

        pte.set_data(PA2PTE(PGROUNDDOWN(riscv::virt_to_phy_address(reinterpret_cast<uint64>(new_page)))) |
                     new_flags |
                     riscv::PteEnum::pte_valid_m);
        flush_riscv_user_page(page_va);
        k_pmm.free_page(old_page);
        return 0;
#elif defined(LOONGARCH)
        if (pte.is_null() || !pte.is_valid() || !pte.is_user_plv() || !pte_is_cow(pte))
        {
            return -1;
        }

        uint64 old_pa = reinterpret_cast<uint64>(pte.pa());
        void *old_page = page_pa_to_kernel_ptr(old_pa);
        if (!k_pmm.is_managed_page(old_page))
        {
            return -1;
        }

        uint64 new_flags = (pte.get_flags() | PTE_W | PTE_D | PTE_P | PTE_U | PTE_MAT) & ~PTE_COW;
        uint16 refcount = k_pmm.page_ref_count(old_page);
        if (refcount == 0)
        {
            return -1;
        }

        if (object_private_mapping &&
            cow_vm->private_page_overlay != nullptr)
        {
            auto overlay = cow_vm->private_page_overlay->find(cow_page_index);
            overlay_owned_by_area = overlay != cow_vm->private_page_overlay->end() &&
                                    overlay->second == old_pa;
        }

        if ((!object_private_mapping && refcount == 1) ||
            (object_private_mapping && overlay_owned_by_area && refcount <= 2))
        {
            pte.set_data(PA2PTE(PGROUNDDOWN(old_pa)) |
                         new_flags |
                         PTE_V);
            invalidate_loongarch_user_page_pair(page_va);
            return 0;
        }

        void *new_page = k_pmm.try_alloc_page_uninitialized();
        if (new_page == nullptr)
        {
            return -1;
        }
        memmove(new_page, old_page, PGSIZE);

        if (object_private_mapping)
        {
            if (cow_vm->private_page_overlay == nullptr)
            {
                cow_vm->private_page_overlay = new proc::VmPrivateOverlayMap();
                if (cow_vm->private_page_overlay == nullptr)
                {
                    k_pmm.free_page(new_page);
                    return -1;
                }
            }

            if (!k_pmm.retain_page(new_page))
            {
                k_pmm.free_page(new_page);
                return -1;
            }

            if (overlay_owned_by_area)
            {
                k_pmm.free_page(old_page);
            }
            (*cow_vm->private_page_overlay)[cow_page_index] =
                to_phy(reinterpret_cast<uint64>(new_page));
        }

        pte.set_data(PA2PTE(PGROUNDDOWN(to_phy(reinterpret_cast<uint64>(new_page)))) |
                     new_flags |
                     PTE_V);
        invalidate_loongarch_user_page_pair(page_va);
        k_pmm.free_page(old_page);
        return 0;
#else
        return -1;
#endif
    }

    void VirtualMemoryManager::vmunmap(PageTable &pt,
                                       uint64 va,
                                       uint64 npages,
                                       int do_free,
                                       UnmapTlbMode tlb_mode)
    {
        F7LY_PERF_ADD(VmunmapCall, 1);
        F7LY_PERF_ADD(VmunmapPages, npages);
        if (tlb_mode == UnmapTlbMode::SkipInactiveFinalTeardown)
        {
            F7LY_PERF_ADD(TeardownUnmapPages, npages);
        }
        // printfCyan("vmunmap: va: %p, npages: %d, do_free: %d\n", va, npages, do_free);
        uint64 a;
        Pte pte;

        /*
         * 单次保留 256 个待释放物理页只占 2 KiB 内核栈，仍明显低于 16 KiB
         * 进程内核栈上限。RISC-V 超过 64 页时本地已退化为一次全 TLB
         * sfence，LoongArch 对任意范围也都会全量失效；扩大批次不会增加
         * 单次硬件失效成本，却能让 jemalloc 大 arena 的 MADV_DONTNEED
         * 少做最多四分之三的跨核 IPI 与确认等待。
        */
        constexpr int k_unmap_batch_pages = 256;
        void *pending[k_unmap_batch_pages]{};
        int pending_count = 0;
        uint64 pending_start = 0;
        uint64 pending_end = 0;

        auto flush_pending = [&]() {
            if (pending_count == 0)
            {
                return;
            }
            if (tlb_mode == UnmapTlbMode::Invalidate)
            {
                // PTE 已全部撤销；同步等待可能运行该地址空间的 CPU 失效后，
                // 才允许这些物理页回到 buddy 并被其它地址空间复用。
                flush_user_pt_range(pt, pending_start, pending_end - pending_start);
            }
            // 最终销毁快路径由 ProcessMemoryManager 在 ref_count==0、active
            // CPU mask==0 后显式开启；对应 ASID 在全核屏障前不会被复用。
            k_pmm.release_pages_batch(pending, static_cast<uint32>(pending_count));
            pending_count = 0;
        };

        if ((va % PGSIZE) != 0)
            panic("vmunmap: not aligned");

        for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
        {
#ifdef RISCV
            bool is_fixed_special_page = (a == TRAMPOLINE || a == SIG_TRAMPOLINE || a == TRAPFRAME);
#elif defined(LOONGARCH)
            bool is_fixed_special_page = (a == SIG_TRAMPOLINE || a == TRAPFRAME);
#endif
            bool is_user_trapframe_page = a >= USER_TRAPFRAME_BASE && a < USER_TRAPFRAME_TOP;
            bool is_reserved_page = is_fixed_special_page || is_user_trapframe_page;
            // 特殊页只允许由它们的专用生命周期路径按单页、无物理页释放地拆除。
            // usertrapret 会在 PCB 槽位复用时替换对应 trapframe；free_pagetable 则会遍历全部槽位。
            bool explicit_reserved_cleanup = npages == 1 && do_free == 0 &&
                                             (is_fixed_special_page || is_user_trapframe_page);
            if (is_reserved_page && !explicit_reserved_cleanup)
            {
                // 这些保留页由页表创建/释放路径专门管理；普通区间回滚不能顺手清掉，
                // 否则下一次 usertrapret 会发现 TRAMPOLINE/SIG_TRAMPOLINE 缺失。
                continue;
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
            void *old_page = do_free
                                 ? page_pa_to_kernel_ptr(reinterpret_cast<uint64>(pte.pa()))
                                 : nullptr;
            pte.clear_data();

            if (pending_count == 0)
            {
                pending_start = a;
            }
            pending_end = a + PGSIZE;
            pending[pending_count++] = old_page;
            if (pending_count == k_unmap_batch_pages)
            {
                flush_pending();
            }
        }
        flush_pending();
    }

    void VirtualMemoryManager::vmunmap_sparse(PageTable &pt,
                                              const uint64 *addresses,
                                              uint32 count,
                                              int do_free,
                                              UnmapTlbMode tlb_mode)
    {
        F7LY_PERF_ADD(VmunmapCall, 1);
        F7LY_PERF_ADD(VmunmapSparsePages, count);
        if (tlb_mode == UnmapTlbMode::SkipInactiveFinalTeardown)
        {
            F7LY_PERF_ADD(TeardownUnmapPages, count);
        }
        constexpr uint32 k_unmap_batch_pages = 256;
        if (addresses == nullptr || count == 0)
        {
            return;
        }
        if (count > k_unmap_batch_pages)
        {
            panic("vmunmap_sparse: too many pages: %u", count);
        }

        void *pending[k_unmap_batch_pages]{};
        uint32 pending_count = 0;
        uint64 flush_start = ~0ULL;
        uint64 flush_end = 0;

        for (uint32 index = 0; index < count; ++index)
        {
            const uint64 va = addresses[index];
            if ((va % PGSIZE) != 0)
            {
                panic("vmunmap_sparse: not aligned va=%p", va);
            }

#ifdef RISCV
            const bool is_fixed_special_page =
                va == TRAMPOLINE || va == SIG_TRAMPOLINE || va == TRAPFRAME;
#elif defined(LOONGARCH)
            const bool is_fixed_special_page =
                va == SIG_TRAMPOLINE || va == TRAPFRAME;
#endif
            const bool is_user_trapframe_page =
                va >= USER_TRAPFRAME_BASE && va < USER_TRAPFRAME_TOP;
            if (is_fixed_special_page || is_user_trapframe_page)
            {
                // 稀疏接口服务普通 resident overlay；固定映射仍由其专用单页
                // 生命周期路径拆除，避免一个坏索引顺手破坏 trap 返回环境。
                continue;
            }

            Pte pte = pt.walk(va, false);
            if (pte.is_null() || !pte.is_valid())
            {
                continue;
            }

            pending[pending_count++] = do_free
                                           ? page_pa_to_kernel_ptr(
                                                 reinterpret_cast<uint64>(pte.pa()))
                                           : nullptr;
            pte.clear_data();
            if (va < flush_start)
            {
                flush_start = va;
            }
            if (va + PGSIZE > flush_end)
            {
                flush_end = va + PGSIZE;
            }
        }

        if (pending_count == 0)
        {
            return;
        }
        if (tlb_mode == UnmapTlbMode::Invalidate)
        {
            // 地址无序时按包围区间保守失效；RISC-V 大于 64 页会自动退化为
            // 单次 ASID 全失效，仍远少于逐页 shootdown。
            flush_user_pt_range(pt, flush_start, flush_end - flush_start);
        }
        k_pmm.release_pages_batch(pending, pending_count);
    }

    PageTable VirtualMemoryManager::vm_create()
    {
        PageTable pt;
        pt.set_global();

        uint64 addr = (uint64)PhysicalMemoryManager::try_alloc_page();
        if (addr == 0)
            return pt;
        pt.set_base(addr);

        return pt;
    }

    int VirtualMemoryManager::vm_copy(PageTable &old_pt,
                                      PageTable &new_pt,
                                      uint64 start,
                                      uint64 size,
                                      bool defer_parent_tlb_flush,
                                      eastl::vector<proc::CowRollbackRange> *rollback_ranges)
    {
        uint64 va_end;

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

        bool parent_cow_changed = false;
        eastl::vector<proc::CowRollbackRange> local_rollback_ranges;
        auto record_parent_cow_change = [&](uint64 changed_va)
        {
            auto append_range = [&](eastl::vector<proc::CowRollbackRange> &ranges)
            {
                if (!ranges.empty() && ranges.back().end == changed_va)
                {
                    ranges.back().end += PGSIZE;
                    return;
                }
                ranges.push_back(proc::CowRollbackRange{
                    .start = changed_va,
                    .end = changed_va + PGSIZE,
                });
            };
            append_range(local_rollback_ranges);
            if (rollback_ranges != nullptr)
            {
                append_range(*rollback_ranges);
            }
        };
        auto fail_copy = [&]() -> int
        {
            /*
             * 子页表已经由各失败分支撤销。只恢复本次调用中“原本可写、随后被
             * 降级为 COW”的父 PTE；不能用物理页引用计数推断权限，因为同一 mm
             * 内的别名页本来就可能有多个映射引用。
             */
            bool restored = false;
            for (const proc::CowRollbackRange &range : local_rollback_ranges)
            {
                for (uint64 rollback_va = range.start;
                     rollback_va < range.end;
                     rollback_va += PGSIZE)
                {
                    Pte rollback_pte = old_pt.walk(rollback_va, false);
                    if (rollback_pte.is_null() || !rollback_pte.is_valid())
                    {
                        continue;
                    }
#ifdef RISCV
                    const uint64 rollback_data = rollback_pte.get_data();
                    if ((rollback_data & k_riscv_pte_cow) != 0)
                    {
                        rollback_pte.set_data(
                            (rollback_data | riscv::PteEnum::pte_writable_m) &
                            ~k_riscv_pte_cow);
                        restored = true;
                    }
#elif defined(LOONGARCH)
                    const uint64 rollback_data = rollback_pte.get_data();
                    if ((rollback_data & PTE_COW) != 0)
                    {
                        rollback_pte.set_data(
                            (rollback_data | PTE_W | PTE_D) & ~PTE_COW);
                        restored = true;
                    }
#endif
                }
            }
            // 即使调用方要求批量失效，失败也会立刻返回到清理路径；此时必须
            // 先撤销父页表中可能仍被旧 TLB 视为可写的翻译，不能把责任留给调用方。
            if (restored)
            {
                flush_user_pt_range(old_pt, copy_start, va_end - copy_start);
            }
            return -1;
        };

        constexpr uint32 k_cow_retain_batch_pages = 32;
        struct CowCopyCandidate
        {
            Pte pte;
            uint64 va = 0;
            uint64 pa = 0;
            uint64 flags = 0;
            void *old_page = nullptr;
        };

        for (uint64 batch_start = copy_start; batch_start < va_end;)
        {
            const uint64 pages_left = (va_end - batch_start) / PGSIZE;
            const uint64 batch_pages = pages_left > k_cow_retain_batch_pages
                                           ? k_cow_retain_batch_pages
                                           : pages_left;
            const uint64 batch_end = batch_start + batch_pages * PGSIZE;
            CowCopyCandidate candidates[k_cow_retain_batch_pages];
            void *pages_to_retain[k_cow_retain_batch_pages];
            uint32 candidate_count = 0;

            for (uint64 scan_va = batch_start; scan_va < batch_end; scan_va += PGSIZE)
            {
                Pte scan_pte = old_pt.walk(scan_va, false);
                if (scan_pte.is_null() || !scan_pte.is_valid())
                {
                    continue;
                }

                CowCopyCandidate &candidate = candidates[candidate_count];
                candidate.pte = scan_pte;
                candidate.va = scan_va;
                candidate.pa = reinterpret_cast<uint64>(scan_pte.pa());
                candidate.flags = scan_pte.get_flags();
                candidate.old_page = page_pa_to_kernel_ptr(candidate.pa);
                pages_to_retain[candidate_count] = candidate.old_page;
                ++candidate_count;
            }

            /*
             * 共享映射已经由 ProcessMemoryManager 按 VMA/VmObject 整段处理。
             * vm_copy 只覆盖私有页，并把同一小批页的 PMM 引用计数放在一次
             * 临界区内完成。大进程 fork 不再为每个 4K 页竞争一次 PMM 锁。
             */
            uint64 retained_mask = 0;
            if (defer_parent_tlb_flush)
            {
                // defer 只允许地址空间唯一持有者使用，此时这一批 PTE 在扫描和
                // retain 之间不会被其他线程拆除，可以安全地缩短 PMM 锁次数。
                retained_mask =
                    k_pmm.retain_pages_batch(pages_to_retain, candidate_count);
            }
            else
            {
                // 多线程共享 mm 的 fork 保持逐页 retain；现有即时 TLB 失效语义
                // 同样保留，避免批量窗口放大与并发 munmap/page fault 的竞争。
                for (uint32 index = 0; index < candidate_count; ++index)
                {
                    if (k_pmm.retain_page(pages_to_retain[index]))
                    {
                        retained_mask |= 1ULL << index;
                    }
                }
            }
            auto release_unconsumed_retains = [&](uint32 first)
            {
                for (uint32 pending = first; pending < candidate_count; ++pending)
                {
                    if ((retained_mask & (1ULL << pending)) != 0)
                    {
                        k_pmm.free_page(candidates[pending].old_page);
                    }
                }
            };

            for (uint32 index = 0; index < candidate_count; ++index)
            {
                CowCopyCandidate &candidate = candidates[index];
                Pte pte = candidate.pte;
                const uint64 va = candidate.va;
                const uint64 pa = candidate.pa;
                const uint64 flags = candidate.flags;
                void *old_page = candidate.old_page;

                if ((retained_mask & (1ULL << index)) != 0)
                {
                    uint64 child_flags = flags;
                    uint64 original_data = pte.get_data();
#ifdef RISCV
                    if ((flags & riscv::PteEnum::pte_writable_m) != 0 ||
                        (flags & k_riscv_pte_cow) != 0)
                    {
                        child_flags = (flags & ~riscv::PteEnum::pte_writable_m) | k_riscv_pte_cow;
                        if ((flags & riscv::PteEnum::pte_writable_m) != 0)
                        {
                            record_parent_cow_change(va);
                            pte.set_data((original_data & ~riscv::PteEnum::pte_writable_m) |
                                         k_riscv_pte_cow);
                            parent_cow_changed = true;
                            if (!defer_parent_tlb_flush)
                            {
                                flush_user_pt_range(old_pt, va, PGSIZE);
                            }
                        }
                    }
#elif defined(LOONGARCH)
                    if ((flags & (PTE_W | PTE_D)) != 0 || (original_data & PTE_COW) != 0)
                    {
                        child_flags = (flags & ~(PTE_W | PTE_D)) | PTE_COW;
                        if ((flags & (PTE_W | PTE_D)) != 0)
                        {
                            // 父子页表都降为只读 COW，之后由写异常拆页。
                            record_parent_cow_change(va);
                            pte.set_data((original_data & ~(PTE_W | PTE_D)) | PTE_COW);
                            parent_cow_changed = true;
                            if (!defer_parent_tlb_flush)
                            {
                                flush_user_pt_range(old_pt, va, PGSIZE);
                            }
                        }
                    }
#endif

                    if (!map_pages(new_pt, va, PGSIZE, pa, child_flags))
                    {
                        k_pmm.free_page(old_page);
                        pte.set_data(original_data);
                        release_unconsumed_retains(index + 1);
                        vmunmap(new_pt, 0, va / PGSIZE, 1);
                        return fail_copy();
                    }
                    continue;
                }

                /*
                 * 非 PMM 管理页无法引用计数，只能保留物理复制回退路径。
                 * 受 PMM 管理的页若 retain 失败，则引用计数不是已经损坏为 0，
                 * 就是到达 UINT16_MAX。此时逐映射复制会破坏父地址空间中的
                 * 物理别名关系；fork 必须原子失败并由 fail_copy() 撤销父权限。
                 */
                if (k_pmm.is_managed_page(old_page))
                {
                    release_unconsumed_retains(index + 1);
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return fail_copy();
                }

                void *new_page = mem::PhysicalMemoryManager::try_alloc_page_uninitialized();
                if (new_page == nullptr)
                {
                    release_unconsumed_retains(index + 1);
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return fail_copy();
                }
                memmove(new_page, old_page, PGSIZE);
                if (!map_pages(new_pt, va, PGSIZE, reinterpret_cast<uint64>(new_page), flags))
                {
                    k_pmm.free_page(new_page);
                    release_unconsumed_retains(index + 1);
                    vmunmap(new_pt, 0, va / PGSIZE, 1);
                    return fail_copy();
                }
            }

            batch_start = batch_end;
        }
        return parent_cow_changed ? 1 : 0;
    }

    void VirtualMemoryManager::uvmclear(PageTable &pt, uint64 va)
    {
        Pte pte = pt.walk(va, 0);
#ifdef RISCV
        if (pte.is_valid())
            pte.set_data(pte.get_data() & ~riscv::PteEnum::pte_user_m);
#elif defined(LOONGARCH)
        if (pte.is_valid())
            pte.set_data(pte.get_data() & ~loongarch::pte_plv_m); // 清除用户 PLV
#endif
        flush_user_pt_range(pt, PGROUNDDOWN(va), PGSIZE);
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
            pa = (uint64)k_pmm.try_alloc_page();
            // printfCyan("[vmalloc] alloc page: %p\n", pa);
            if (pa == 0)
            {
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
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
            mem = k_pmm.try_alloc_page();
            if (mem == 0)
            {
                // printfCyan("[vmalloc] alloc page failed, oldsz: %p, newsz: %p\n", oldsz, newsz);
                uvmdealloc(pt, a, oldsz);
                return 0;
            }
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
            panic("[vmm] kvmmap failed: va=%p pa=%p size=%p perms=%p",
                  va, pa, sz, perms);
        }
    }

    PageTable VirtualMemoryManager::kvmmake()
    {
        PageTable pt;
        pt.set_global();
        pt.set_base((uint64)k_pmm.alloc_page());
        memset((void *)pt.get_base(), 0, PGSIZE);
#ifdef RISCV
        const uint64 kernel_base = mem::kernel_image_start_address();
        // 页表只消费当前画像声明的 typed MMIO 区间。新增开发板时由其平台
        // 配置提供资源表，VMM 不再保存 UART/PLIC/VirtIO 地址副本。
        for (const platform::MmioRegion &region :
             riscv::board::k_kernel_mmio_regions)
        {
            kvmmap(pt, region.physical_base, region.physical_base,
                   region.size, PTE_R | PTE_W);
        }
        // map kernel text executable and read-only.
        if (reinterpret_cast<uint64>(etext) <= kernel_base)
        {
            panic("[vmm] invalid RISC-V kernel text range: base=%p etext=%p",
                  kernel_base, etext);
        }
        kvmmap(pt, kernel_base, kernel_base,
               reinterpret_cast<uint64>(etext) - kernel_base, PTE_R | PTE_X);
        // map kernel data and the physical RAM we'll make use of.
        const uint64 linear_top = k_pmm.get_kernel_linear_top();
        if (linear_top <= (uint64)etext)
        {
            panic("[vmm] invalid kernel linear top %p vs etext %p", linear_top, etext);
        }
        kvmmap(pt, (uint64)etext, (uint64)etext,
               linear_top - (uint64)etext, PTE_R | PTE_W);

        // 正常情况下 linear mapping 已覆盖完整 RAM（包括只映射、不分配的
        // DTB/initrd 页面）。若固件把 blob 放在 RAM 描述之外，只补映射位于
        // 线性区两端之外的页面，避免与已有 PTE 重叠。
        auto map_reserved_identity = [&](uint64 raw_start, uint64 raw_end,
                                         const char *name) {
            if (raw_start == 0 || raw_end <= raw_start)
            {
                return;
            }
            const uint64 map_start = PGROUNDDOWN(raw_start);
            const uint64 map_end = PGROUNDUP(raw_end);
            if (map_start < kernel_base)
            {
                const uint64 prefix_end = map_end < kernel_base ? map_end : kernel_base;
                if (prefix_end > map_start)
                {
                    kvmmap(pt, map_start, map_start,
                           prefix_end - map_start, PTE_R | PTE_W);
                }
            }
            if (map_end > linear_top)
            {
                const uint64 suffix_start = map_start > linear_top
                                                ? map_start
                                                : linear_top;
                if (map_end > suffix_start)
                {
                    kvmmap(pt, suffix_start, suffix_start,
                           map_end - suffix_start, PTE_R | PTE_W);
                }
            }
            printfGreen("[vmm] reserved %s mapped/covered at %p-%p\n",
                        name, map_start, map_end);
        };

        if (k_initrd_start != 0 && k_initrd_end > k_initrd_start)
        {
            map_reserved_identity(k_initrd_start, k_initrd_end, "initrd");
        }

        if (k_dtb_addr != 0)
        {
            uint64 dtb_size = DtbManager::get_dtb_size();
            if (dtb_size == 0)
            {
                panic("[vmm] validated DTB has no usable totalsize");
            }
            if (k_dtb_addr > ~0ULL - dtb_size)
            {
                panic("[vmm] DTB mapping overflow");
            }
            map_reserved_identity(k_dtb_addr, k_dtb_addr + dtb_size, "dtb");
        }

        // trampoline 必须在内核页表与用户页表中保持相同虚拟地址；
        // 用户侧映射由 ProcessMemoryManager 建立。
        kvmmap(pt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

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
        if (heap_size == 0 || heap_start > UINT64_MAX - heap_size)
        {
            panic("[vmm] invalid heap mapping range: start=%p size=%p",
                  heap_start, heap_size);
        }
        const uint64 heap_end = heap_start + heap_size;
        if (heap_start < low_map_start_va)
        {
            panic("[vmm] heap overlaps kernel text: heap=%p low-map-start=%p",
                  heap_start, low_map_start_va);
        }
        if (heap_end > low_map_top)
        {
            // heap 可能跨过包含内核的低端 RAM 边界。低端前缀已有 PTE，
            // 这里只补尚未覆盖的后缀；重复映射会触发 map_pages remap panic。
            const uint64 suffix_start = heap_start > low_map_top
                                            ? heap_start
                                            : low_map_top;
            kvmmap(pt, suffix_start & (~(DMWIN_MASK)), suffix_start,
                   heap_end - suffix_start, PTE_R | PTE_W);
        }

        // DtbManager 始终通过缓存 DMW 别名访问 LoongArch DTB。DMW 不依赖
        // 页表，因而这里不能再把 DTB 的低地址重复映射一次：U-Boot 若把
        // blob 放在已经覆盖的 RAM 内，重复 kvmmap 会把合法启动变成冲突 panic。

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
            uint64 copy_size = util::min(sz - src_offset, PGSIZE);
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
            uint64 copy_size = util::min(sz - src_offset, PGSIZE);
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

    int VirtualMemoryManager::protectpages(PageTable &pt,
                                            uint64 va,
                                            uint64 size,
                                            int prot,
                                            bool is_vma,
                                            bool *pte_changed)
    {
        uint64 a, last;
        Pte pte;

        if (pte_changed != nullptr)
        {
            *pte_changed = false;
        }

        // printf("[protectpages] va: %p, size: %p, perm: %p, is_vma: %d\n", va, size, perm, is_vma);

        last = PGROUNDDOWN(va + size - 1);

        for (a = PGROUNDDOWN(va); a != last + PGSIZE; a += PGSIZE)
        {
            // VMA 上下文中 mprotect() 已经更新了 VMA 元数据；尚未 fault 的懒分配页
            // 不需要为了改权限提前创建页表层级，后续缺页会按新的 prot 建立 PTE。
            pte = pt.walk(a, is_vma ? 0 : 1);
            if (pte.is_null())
            {
                if (is_vma)
                {
                    continue;
                }
                return -1;
            }

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
                if (!should_preserve_cow_without_write(pt, a, pte, is_vma))
                {
                    new_data &= ~k_riscv_pte_cow;
                }
                if (prot & PROT_READ)
                    new_data |= riscv::PteEnum::pte_readable_m;
                if (prot & PROT_WRITE)
                {
                    new_data &= ~k_riscv_pte_cow;
                    if (should_write_protect_as_cow(pt, a, pte, is_vma))
                    {
                        // 只有私有 VMA 的共享物理页需要保留 COW；MAP_SHARED 必须恢复真实写权限。
                        new_data |= k_riscv_pte_cow;
                    }
                    else
                    {
                        new_data |= riscv::PteEnum::pte_writable_m;
                    }
                }
                if (prot & PROT_EXEC)
                    new_data |= riscv::PteEnum::pte_executable_m;
                new_data |= riscv::PteEnum::pte_valid_m | riscv::PteEnum::pte_user_m;
#elif defined(LOONGARCH)
                // LoongArch 的读/执行权限是“禁止位”(NR/NX)，而不是正向的 R/X 位。
                // mprotect(PROT_NONE) / 取消执行权限都必须显式写回 NR/NX。
                uint64 new_data = old_data & ~(PTE_W | PTE_D | PTE_NR | PTE_NX | PTE_PLV);
                if (!should_preserve_cow_without_write(pt, a, pte, is_vma))
                {
                    new_data &= ~PTE_COW;
                }
                new_data |= PTE_V | PTE_U;
                if (prot & PROT_WRITE)
                {
                    new_data &= ~PTE_COW;
                    if (should_write_protect_as_cow(pt, a, pte, is_vma))
                    {
                        // 只有私有 VMA 的共享物理页需要保留 COW；MAP_SHARED 必须恢复真实写权限。
                        new_data |= PTE_COW;
                    }
                    else
                    {
                        new_data |= PTE_W | PTE_D;
                    }
                }
                if (!(prot & PROT_READ))
                    new_data |= PTE_NR;
                if (!(prot & PROT_EXEC))
                    new_data |= PTE_NX;
#endif
                if (new_data != old_data)
                {
                    pte.set_data(new_data);
                    if (pte_changed != nullptr)
                    {
                        *pte_changed = true;
                    }
                }
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

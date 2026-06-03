/**
 * @file process_memory_manager.cc
 * @brief 进程内存管理器实现
 *
 * 实现进程内存管理器的所有功能，提供统一的内存管理接口。
 * 将原本散落在proc_manager.cc中的内存管理逻辑重构到这里。
 *
 * 统一管理说明：
 * - 所有内存释放统一通过 free_all_memory() 进行
 * - free_heap_memory() 内部调用 cleanup_heap_to_size(0)
 * - get_total_memory_usage() 返回缓存值，calculate_total_memory_size() 实时计算
 * - verify_all_memory_consistency() 包含 verify_memory_consistency 的核心逻辑
 * - get_total_program_memory() 保留为API兼容性，功能包含在 calculate_total_memory_size() 中
 */
#include "proc_manager.hh"
#include "process_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "physical_memory_manager.hh"
#include "klib.hh"
#include "printer.hh"
#include "platform.hh" // 为MAX/MIN宏
#include "memlayout.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/vfs_utils.hh"
#include "shm/shm_manager.hh"

// 外部符号声明
extern char trampoline[];     // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

namespace proc
{
    namespace
    {
        constexpr uint64 k_mmap_min_base = 0x10000000ULL;
        constexpr uint64 k_mmap_guard_gap = 16 * PGSIZE;
        constexpr uint64 k_mmap_upper_guard = 256 * PGSIZE;
#ifdef RISCV
        constexpr uint64 k_min_kernel_object_ptr = KERNBASE;
#elif defined(LOONGARCH)
        constexpr uint64 k_min_kernel_object_ptr = PHYSBASE;
#endif
        constexpr uint32 k_max_reasonable_file_refcnt = num_process * max_open_files;

        inline uint64 align_up_with_granularity(uint64 value, uint64 alignment)
        {
            if (alignment == 0)
            {
                return value;
            }
            uint64 remainder = value % alignment;
            if (remainder == 0)
            {
                return value;
            }
            return value + (alignment - remainder);
        }

        inline void reset_vma_entry(vma &entry)
        {
            memset(&entry, 0, sizeof(vma));
            entry.backing_kind = VMA_BACKING_NONE;
            entry.backing_shmid = -1;
            entry.backing_base = 0;
            entry.has_resident_pages = false;
            entry.wipe_on_fork = false;
        }

        inline void *user_page_kernel_ptr(uint64 pa)
        {
#ifdef LOONGARCH
            return reinterpret_cast<void *>(to_vir(pa));
#else
            return reinterpret_cast<void *>(pa);
#endif
        }

        void wipe_child_vma_pages(mem::PageTable &child_pt, uint64 start, uint64 len)
        {
            if (len == 0)
            {
                return;
            }

            uint64 begin = PGROUNDDOWN(start);
            uint64 end = PGROUNDUP(start + len);
            if (end < begin)
            {
                return;
            }

            for (uint64 va = begin; va < end; va += PGSIZE)
            {
                mem::Pte pte = child_pt.walk(va, false);
                if (pte.is_null() || !pte.is_valid())
                {
                    continue;
                }
                if (!pte.is_writable())
                {
                    // fork COW 后 child 的私有页可能暂时只读；清零前必须先拆页，
                    // 否则会把父进程共享的物理页也一起擦掉。
                    if (mem::k_vmm.resolve_cow_page(child_pt, va) != 0)
                    {
                        continue;
                    }
                    pte = child_pt.walk(va, false);
                    if (pte.is_null() || !pte.is_valid())
                    {
                        continue;
                    }
                }
                memset(user_page_kernel_ptr((uint64)pte.pa()), 0, PGSIZE);
            }
        }

        inline bool is_shared_backed_vma(const vma &entry)
        {
            return entry.used && entry.backing_kind == VMA_BACKING_SHM && entry.backing_shmid >= 0;
        }

        inline uint64 pte_data_kernel_addr(mem::Pte &pte)
        {
            uint64 pa = reinterpret_cast<uint64>(pte.pa());
#ifdef LOONGARCH
            pa = to_vir(pa);
#endif
            return pa;
        }

        inline bool pte_allows_user_access(mem::Pte &pte)
        {
#ifdef RISCV
            return pte.is_user();
#elif defined(LOONGARCH)
            return pte.is_user_plv();
#endif
        }

        inline uint64 build_user_pte_flags_from_vma(const vma &entry)
        {
            uint64 pte_flags = 0;
#ifdef RISCV
            pte_flags = riscv::PteEnum::pte_user_m;
            if (entry.prot & PROT_READ)
            {
                pte_flags |= riscv::PteEnum::pte_readable_m;
            }
            if (entry.prot & PROT_WRITE)
            {
                pte_flags |= riscv::PteEnum::pte_writable_m;
                pte_flags |= riscv::PteEnum::pte_readable_m;
            }
            if (entry.prot & PROT_EXEC)
            {
                pte_flags |= riscv::PteEnum::pte_executable_m;
            }
#elif defined(LOONGARCH)
            pte_flags = PTE_U | PTE_P | PTE_MAT;
            if (entry.prot & PROT_READ)
            {
                pte_flags |= PTE_R;
            }
            if (entry.prot & PROT_WRITE)
            {
                pte_flags |= PTE_W | PTE_D;
            }
            if (entry.prot & PROT_EXEC)
            {
                pte_flags |= PTE_X;
            }
#endif
            return pte_flags;
        }

        bool remap_shared_backed_vma(ProcessMemoryManager &target_mm, const vma &entry)
        {
            if (!is_shared_backed_vma(entry))
            {
                return true;
            }

            if (entry.prot == PROT_NONE)
            {
                return true;
            }

            shm::shm_segment seg = shm::k_smm.get_seg_info(entry.backing_shmid);
            if (seg.shmid < 0)
            {
                printfRed("[clone_for_fork] invalid shared backing shmid=%d addr=%p len=%d\n",
                          entry.backing_shmid, (void *)entry.addr, entry.len);
                return false;
            }

            uint64 map_start = PGROUNDDOWN(entry.backing_base != 0 ? entry.backing_base : entry.addr);
            uint64 map_end = PGROUNDUP(entry.addr + (uint64)entry.len);
            if (map_end < map_start)
            {
                printfRed("[clone_for_fork] shared VMA range overflow shmid=%d addr=%p len=%d\n",
                          entry.backing_shmid, (void *)entry.addr, entry.len);
                return false;
            }

            uint64 pte_flags = build_user_pte_flags_from_vma(entry);
            for (uint64 va = map_start; va < map_end; va += PGSIZE)
            {
                uint64 page_offset = va - map_start;
                if (page_offset >= seg.real_size)
                {
                    printfRed("[clone_for_fork] shared VMA remap overflow shmid=%d va=%p offset=%p real_size=%p\n",
                              entry.backing_shmid, (void *)va, (void *)page_offset, (void *)seg.real_size);
                    return false;
                }

                uint64 pa = seg.phy_addrs + page_offset;
                if (!mem::k_vmm.map_pages(target_mm.pagetable, va, PGSIZE, pa, pte_flags))
                {
                    printfRed("[clone_for_fork] remap shared VMA failed shmid=%d va=%p pa=%p\n",
                              entry.backing_shmid, (void *)va, (void *)pa);
                    return false;
                }
            }

            return true;
        }

        inline bool is_same_shared_backing(const vma &lhs, const vma &rhs)
        {
            return is_shared_backed_vma(lhs) &&
                   is_shared_backed_vma(rhs) &&
                   lhs.backing_shmid == rhs.backing_shmid &&
                   lhs.backing_base == rhs.backing_base;
        }

        inline bool has_other_shared_backing_fragment(const VMA &vmas, int skip_index, const vma &target)
        {
            for (int i = 0; i < NVMA; ++i)
            {
                if (i == skip_index)
                {
                    continue;
                }
                if (is_same_shared_backing(vmas._vm[i], target))
                {
                    return true;
                }
            }
            return false;
        }

        inline bool find_shared_backed_vma_covering(const VMA &vmas, uint64 addr, void **start_addr, size_t *size)
        {
            for (int i = 0; i < NVMA; ++i)
            {
                const vma &entry = vmas._vm[i];
                if (!is_shared_backed_vma(entry))
                {
                    continue;
                }

                uint64 vm_start = PGROUNDDOWN(entry.addr);
                uint64 vm_end = PGROUNDUP(entry.addr + entry.len);
                if (addr < vm_start || addr >= vm_end)
                {
                    continue;
                }

                if (start_addr != nullptr)
                {
                    *start_addr = (void *)vm_start;
                }
                if (size != nullptr)
                {
                    *size = vm_end - vm_start;
                }
                return true;
            }
            return false;
        }

        inline int count_live_mm_holders(ProcessMemoryManager *target)
        {
            if (target == nullptr)
            {
                return 0;
            }

            int holders = 0;
            for (proc::Pcb &pcb : proc::k_proc_pool)
            {
                if (pcb._state == proc::UNUSED)
                {
                    continue;
                }
                if (pcb.get_memory_manager() == target)
                {
                    ++holders;
                }
            }
            return holders;
        }

        inline bool is_reasonable_program_section(const program_section_desc &section)
        {
            if (section._sec_size == 0)
            {
                return false;
            }
            uint64 start = (uint64)section._sec_start;
            uint64 end = start + section._sec_size;
            if (end <= start)
            {
                return false;
            }
            if (start >= TRAPFRAME || end > TRAPFRAME)
            {
                return false;
            }
            return true;
        }

#ifdef LOONGARCH
        bool ensure_user_pagetable_hierarchy(mem::PageTable &pt, uint64 start, uint64 size)
        {
            if (size == 0)
            {
                return true;
            }

            uint64 range_start = PGROUNDDOWN(start);
            uint64 range_end = PGROUNDUP(start + size);
            if (range_end < range_start || range_start >= TRAPFRAME || range_end > TRAPFRAME)
            {
                printfRed("[clone_for_fork] invalid hierarchy-prebuild range [%p, %p)\n",
                          (void *)range_start, (void *)range_end);
                return false;
            }

            // LoongArch 的 tlbr refill 入口要求页表中间层级已经存在。
            // fork/clone 只复制“已驻留页”时，像 guarded stack 这种合法但尚未驻留的页
            // 会在子进程第一次触达时直接踩进架构相关异常，而不是走正常懒缺页。
            for (uint64 va = range_start; va < range_end; va += PGSIZE)
            {
                mem::Pte pte_slot = pt.walk(va, true);
                if (pte_slot.is_null())
                {
                    printfRed("[clone_for_fork] prebuild pagetable hierarchy failed for va=%p\n",
                              (void *)va);
                    return false;
                }
            }

            return true;
        }
#endif

        inline bool is_probably_kernel_object_ptr(const void *ptr)
        {
            return (uint64)ptr >= k_min_kernel_object_ptr;
        }

        inline bool is_kernel_mapped_range(uint64 addr, uint64 size)
        {
            if (addr < k_min_kernel_object_ptr || size == 0)
            {
                return false;
            }

            uint64 end = addr + size - 1;
            if (end < addr)
            {
                return false;
            }

            return mem::k_pagetable.kwalk_addr(addr) != 0 &&
                   mem::k_pagetable.kwalk_addr(end) != 0;
        }

        inline bool is_probably_live_file_object(fs::file *file_obj)
        {
            if (file_obj == nullptr)
            {
                return false;
            }

            if (!is_kernel_mapped_range((uint64)file_obj, sizeof(fs::file)))
            {
                return false;
            }

            uint64 vtable_addr = *(uint64 *)file_obj;
            if (!is_kernel_mapped_range(vtable_addr, sizeof(void *)))
            {
                return false;
            }

            uint32 refcnt = file_obj->refcnt;
            return refcnt > 0 && refcnt <= k_max_reasonable_file_refcnt;
        }

        inline bool is_reasonable_user_vma(const vma &entry)
        {
            if (!entry.used)
            {
                return false;
            }
            if (entry.len <= 0)
            {
                return false;
            }
            uint64 start = entry.addr;
            uint64 end = start + (uint64)entry.len;
            if (end <= start)
            {
                return false;
            }
            if (start >= MAXVA || end > MAXVA)
            {
                return false;
            }
            return true;
        }

        inline int release_shared_backed_vma(ProcessMemoryManager &mm,
                                             int vma_index,
                                             const vma &vm_entry,
                                             bool check_validity,
                                             const char *context)
        {
            uint64 va_start = PGROUNDDOWN(vm_entry.addr);
            uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);

            // 共享映射的物理页由共享段后端持有，这里只撤销当前片段的页表映射。
            mm.safe_vmunmap(va_start, va_end, check_validity);

            // 同一条共享段经过 split/trim 之后可能散成多个 VMA 片段。
            // 只有最后一个片段离开时，才真正 detach 那条共享段附件记录。
            if (has_other_shared_backing_fragment(mm.vma_data, vma_index, vm_entry))
            {
                return 0;
            }

            int detach_result = shm::k_smm.detach_seg((void *)vm_entry.backing_base);
            if (detach_result != 0)
            {
                printfRed("ProcessMemoryManager: %s detach shared VMA failed, addr=%p shmid=%d ret=%d\n",
                          context,
                          (void *)vm_entry.backing_base,
                          vm_entry.backing_shmid,
                          detach_result);
            }
            return detach_result;
        }
    } // namespace


    ProcessMemoryManager::ProcessMemoryManager()
        : prog_section_count(0), heap_start(0), heap_end(0), mmap_cursor(0), shared_vm(false),
          total_memory_size(0), ref_count(1)
    {
        // 初始化内存锁
        memory_lock.init("process_memory_lock_guard", "process_memory_lock");

        // 初始化程序段数组
        for (int i = 0; i < max_program_section_num; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }

        // 初始化VMA数据
        // 阶段1：移除VMA的分散引用计数，统一使用ProcessMemoryManager的引用计数
        for (int i = 0; i < NVMA; i++)
        {
            reset_vma_entry(vma_data._vm[i]);
        }
    }

    ProcessMemoryManager::~ProcessMemoryManager()
    {
        // 析构函数中不执行清理操作，避免双重释放
        // 清理应该通过显式调用free_all_memory()来完成
    }

    void ProcessMemoryManager::get()
    {
        ref_count.fetch_add(1, eastl::memory_order_relaxed);
    }

    bool ProcessMemoryManager::put()
    {
        int old_count = ref_count.fetch_sub(1, eastl::memory_order_acq_rel);
        if (old_count <= 1)
        {
            // 引用计数降至0或以下，需要清理
            return true;
        }
        return false;
    }

    int ProcessMemoryManager::get_ref_count() const
    {
        return ref_count.load(eastl::memory_order_acquire);
    }

    void ProcessMemoryManager::lock_memory()
    {
        memory_lock.acquire();
    }

    void ProcessMemoryManager::unlock_memory()
    {
        memory_lock.release();
    }

    ProcessMemoryManager *ProcessMemoryManager::share_for_thread()
    {
        // 线程共享：增加引用计数并返回当前对象
        get();
        shared_vm = true; // 标记为共享虚拟内存
        return this;
    }

    ProcessMemoryManager *ProcessMemoryManager::clone_for_fork()
    {
        // 进程复制：创建新的内存管理器并深拷贝内容
        ProcessMemoryManager *new_mgr = new ProcessMemoryManager();

        // 为新进程创建页表
        if (!new_mgr->create_pagetable())
        {
            panic("[clone for fork] create_pagetable faol");
            delete new_mgr;
            return nullptr;
        }
        // printf("[clone_for_fork] start clone prog_section\n");

        // 复制程序段描述时顺便做一层元数据清洗，避免损坏的高地址保留页再次被克隆。
        new_mgr->prog_section_count = 0;
        for (int i = 0; i < prog_section_count && i < max_program_section_num; i++)
        {
            if (!is_reasonable_program_section(prog_sections[i]))
            {
                printfRed("[clone_for_fork] skip invalid program section %d start=%p size=%p name=%s\n",
                          i,
                          prog_sections[i]._sec_start,
                          (void *)prog_sections[i]._sec_size,
                          prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "(null)");
                continue;
            }
            new_mgr->prog_sections[new_mgr->prog_section_count++] = prog_sections[i];
        }

        // 复制堆信息
        new_mgr->heap_start = heap_start;
        new_mgr->heap_end = heap_end;
        new_mgr->mmap_cursor = mmap_cursor;

        // 复制总内存大小
        new_mgr->total_memory_size = total_memory_size;

        // fork操作不共享虚拟内存，设置为false
        new_mgr->shared_vm = false;

#ifdef LOONGARCH
        // 先把子进程合法用户区间的页表层级补齐，但不提前建立叶子映射。
        // 这样 LoongArch 的 tlbr refill 能稳定落到懒缺页路径，同时不破坏 lazy allocation。
        for (int i = 0; i < new_mgr->prog_section_count; ++i)
        {
            uint64 start = (uint64)new_mgr->prog_sections[i]._sec_start;
            uint64 size = new_mgr->prog_sections[i]._sec_size;
            if (!ensure_user_pagetable_hierarchy(new_mgr->pagetable, start, size))
            {
                delete new_mgr;
                return nullptr;
            }
        }

        if (new_mgr->heap_end > new_mgr->heap_start)
        {
            uint64 heap_copy_start = PGROUNDDOWN(new_mgr->heap_start);
            uint64 heap_copy_end = PGROUNDUP(new_mgr->heap_end);
            if (heap_copy_end > heap_copy_start &&
                !ensure_user_pagetable_hierarchy(new_mgr->pagetable,
                                                 heap_copy_start,
                                                 heap_copy_end - heap_copy_start))
            {
                delete new_mgr;
                return nullptr;
            }
        }
#endif

        // 复制进程的所有内存段
        bool copy_success = true;


        // 复制程序段
        for (int i = 0; i < prog_section_count && i < max_program_section_num; i++)
        {
            if (!is_reasonable_program_section(prog_sections[i]))
            {
                continue;
            }
            uint64 start = (uint64)prog_sections[i]._sec_start;
            uint64 size = prog_sections[i]._sec_size;

            if (mem::k_vmm.vm_copy(pagetable, new_mgr->pagetable, start, size) < 0)
            {
                copy_success = false;
                break;
            }
        }

        // 复制堆
        if (copy_success && (heap_end > heap_start))
        {
            uint64 heap_copy_start = PGROUNDDOWN(heap_start);
            uint64 heap_copy_end = PGROUNDUP(heap_end);
            if (heap_copy_end <= heap_copy_start || heap_copy_end > TRAPFRAME)
            {
                printfRed("[clone_for_fork] skip invalid heap range start=%p end=%p\n",
                          (void *)heap_start, (void *)heap_end);
            }
            else if (mem::k_vmm.vm_copy(pagetable, new_mgr->pagetable,
                                        heap_copy_start, heap_copy_end - heap_copy_start) < 0)
            {
                copy_success = false;
            }
        }

        if (!copy_success)
        {
            panic("[clone_from_fork] copy failed");
            delete new_mgr;
            return nullptr;
        }

        // 复制VMA数据时过滤掉明显损坏的条目，避免脏元数据扩散到子进程。
        for (int i = 0; i < NVMA; ++i)
        {
            reset_vma_entry(new_mgr->vma_data._vm[i]);
        }
        for (int i = 0; i < NVMA; ++i)
        {
            if (!vma_data._vm[i].used)
            {
                continue;
            }

            if (!is_reasonable_user_vma(vma_data._vm[i]))
            {
                printfRed("[clone_for_fork] skip invalid VMA %d addr=%p len=%d flags=0x%x prot=0x%x\n",
                          i,
                          (void *)vma_data._vm[i].addr,
                          vma_data._vm[i].len,
                          vma_data._vm[i].flags,
                          vma_data._vm[i].prot);
                continue;
            }

            new_mgr->vma_data._vm[i] = vma_data._vm[i];

#ifdef LOONGARCH
            if (!ensure_user_pagetable_hierarchy(new_mgr->pagetable,
                                                 new_mgr->vma_data._vm[i].addr,
                                                 new_mgr->vma_data._vm[i].len))
            {
                delete new_mgr;
                return nullptr;
            }
#endif

            // 只对文件映射增加引用计数
            if (vma_data._vm[i].vfile != nullptr)
            {
                vma_data._vm[i].vfile->dup(); // 增加引用计数
            }

            // fork 必须保留父进程已经驻留的私有 VMA 页。
            // 动态链接器会在 MAP_PRIVATE 的 libc/ld.so GOT 页上写入重定位结果；
            // 如果这里只复制 VMA 元数据，子进程缺页时会重新从文件读原始 GOT，
            // 导致 _rtld_global 等指针退回 0 并在 glibc __fork 子分支崩溃。
            // 未驻留页仍保持惰性加载；MAP_SHARED/SHM 页由共享后端负责重新映射。
            if (is_shared_backed_vma(vma_data._vm[i]))
            {
                if (!remap_shared_backed_vma(*new_mgr, vma_data._vm[i]))
                {
                    delete new_mgr;
                    return nullptr;
                }
            }
            else
            {
                uint64 vma_start = PGROUNDDOWN(vma_data._vm[i].addr);
                uint64 vma_end = PGROUNDUP(vma_data._vm[i].addr + (uint64)vma_data._vm[i].len);
                if (vma_end < vma_start)
                {
                    printfRed("[clone_for_fork] skip overflow VMA copy %d addr=%p len=%d\n",
                              i, (void *)vma_data._vm[i].addr, vma_data._vm[i].len);
                    continue;
                }

                if (mem::k_vmm.vm_copy(pagetable, new_mgr->pagetable, vma_start, vma_end - vma_start) < 0)
                {
                    printfRed("[clone_for_fork] copy VMA %d failed addr=%p len=%d\n",
                              i, (void *)vma_data._vm[i].addr, vma_data._vm[i].len);
                    delete new_mgr;
                    return nullptr;
                }

                if (vma_data._vm[i].wipe_on_fork)
                {
                    wipe_child_vma_pages(new_mgr->pagetable,
                                         vma_data._vm[i].addr,
                                         static_cast<uint64>(vma_data._vm[i].len));
                }
            }
        }

        return new_mgr;
    }

    /****************************************************************************************
     * 程序段管理接口实现
     ****************************************************************************************/

    int ProcessMemoryManager::add_program_section(void *start, ulong size, const char *name)
    {
        if (prog_section_count >= max_program_section_num)
        {
            panic("add_program_section: too many program sections\n");
            return -1;
        }

        int index = prog_section_count++;
        prog_sections[index]._sec_start = start;
        prog_sections[index]._sec_size = size;
        prog_sections[index]._debug_name = name;

        // 更新总内存大小
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();

        return index;
    }

    void ProcessMemoryManager::remove_program_section(int index)
    {
        if (index < 0 || index >= prog_section_count)
        {
            printfRed("remove_program_section: invalid index %d\n", index);
            return;
        }

        // 移动后续段到前面
        for (int i = index; i < prog_section_count - 1; i++)
        {
            prog_sections[i] = prog_sections[i + 1];
        }

        prog_section_count--;

        // 清理最后一个位置
        prog_sections[prog_section_count]._sec_start = nullptr;
        prog_sections[prog_section_count]._sec_size = 0;
        prog_sections[prog_section_count]._debug_name = nullptr;

        // 更新总内存大小
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();
    }

    void ProcessMemoryManager::clear_all_program_sections_data()
    {
        for (int i = 0; i < prog_section_count; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }
        prog_section_count = 0;

        // 重新计算总内存大小：只包含堆空间
        update_total_memory_size();

        // 验证内存一致性
        verify_memory_consistency();
    }

    void ProcessMemoryManager::reset_memory_sections()
    {
        // 清空所有程序段
        clear_all_program_sections_data();

        // execve/free_all_memory 之后必须把整张 VMA 表重新清零。
        // 仅靠 free_all_vma() 清理“当前 used 的条目”不够，历史上出现过
        // used/len 被脏数据带坏后残留到下一次 execve 的情况，最终表现成幽灵 VMA。
        for (int i = 0; i < NVMA; i++)
        {
            reset_vma_entry(vma_data._vm[i]);
        }

        // 重置堆信息
        heap_start = 0;
        heap_end = 0;
        mmap_cursor = 0;

        // 重置总内存大小
        total_memory_size = 0;
        shared_vm = false;
    }

    uint64 ProcessMemoryManager::get_total_program_memory() const
    {
        // 为API兼容性保留，实现程序段总大小计算
        uint64 total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            total += prog_sections[i]._sec_size;
        }
        return total;
    }

    void ProcessMemoryManager::copy_program_sections(const ProcessMemoryManager &src)
    {
        prog_section_count = 0;
        int src_count = src.prog_section_count;
        if (src_count < 0 || src_count > max_program_section_num)
        {
            printfRed("ProcessMemoryManager: 源程序段计数异常，clamp 到合法范围: %d\n",
                      src_count);
            src_count = src_count < 0 ? 0 : max_program_section_num;
        }
        for (int i = 0; i < src_count; i++)
        {
            if (!is_reasonable_program_section(src.prog_sections[i]))
            {
                printfRed("ProcessMemoryManager: 跳过异常程序段 section=%d start=%p size=%p name=%s\n",
                          i,
                          src.prog_sections[i]._sec_start,
                          (void *)src.prog_sections[i]._sec_size,
                          src.prog_sections[i]._debug_name ? src.prog_sections[i]._debug_name : "(null)");
                continue;
            }
            prog_sections[prog_section_count++] = src.prog_sections[i];
        }

        // 更新总内存大小
        update_total_memory_size();
    }

    void ProcessMemoryManager::free_all_program_sections()
    {
        int section_count = prog_section_count;
        if (section_count < 0 || section_count > max_program_section_num)
        {
            printfRed("ProcessMemoryManager: prog_section_count 异常，clamp 到合法范围: %d\n",
                      section_count);
            section_count = section_count < 0 ? 0 : max_program_section_num;
        }

        // 释放程序段占用的内存
        for (int i = 0; i < section_count; i++)
        {
            if (prog_sections[i]._sec_start == nullptr && prog_sections[i]._sec_size == 0)
            {
                printfYellow("ProcessMemoryManager: 跳过空程序段描述 section=%d\n", i);
                continue;
            }

            if (!is_reasonable_program_section(prog_sections[i]))
            {
                printfRed("ProcessMemoryManager: 程序段地址范围异常，跳过释放 section=%d name=%s start=%p size=%p\n",
                          i,
                          prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "(null)",
                          prog_sections[i]._sec_start,
                          (void *)prog_sections[i]._sec_size);
                continue;
            }

            if (prog_sections[i]._sec_size == 0)
            {
                printfRed("ProcessMemoryManager: 程序段大小为 0，跳过释放 section=%d name=%s start=%p\n",
                          i,
                          prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "(null)",
                          prog_sections[i]._sec_start);
                continue;
            }

            uint64 raw_start = (uint64)prog_sections[i]._sec_start;
            uint64 raw_end = raw_start + prog_sections[i]._sec_size;
            uint64 va_start = PGROUNDDOWN(raw_start);
            uint64 va_end = PGROUNDUP(raw_end);
            safe_vmunmap(va_start, va_end, true);
        }

        // 阶段1：清理ProcessMemoryManager内的程序段描述信息
        for (int i = 0; i < max_program_section_num; i++)
        {
            prog_sections[i]._sec_start = nullptr;
            prog_sections[i]._sec_size = 0;
            prog_sections[i]._debug_name = nullptr;
        }
        prog_section_count = 0;

        // printfGreen("ProcessMemoryManager: program sections freed successfully\n");
    }

    bool ProcessMemoryManager::verify_program_sections_consistency() const
    {
        // 直接计算ProcessMemoryManager中程序段的总大小
        uint64 sections_total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            sections_total += prog_sections[i]._sec_size;
        }

        // 与ProcessMemoryManager维护的总内存大小进行比较
        // 注意：total_memory_size包含程序段+堆，但不包含VMA
        uint64 heap_size = heap_end > heap_start ? heap_end - heap_start : 0;
        uint64 expected_sections_total = total_memory_size - heap_size;

        if (sections_total != expected_sections_total)
        {
            printfRed("ProcessMemoryManager: program sections inconsistency detected\n");
            printfRed("  Sections total: %u, Expected (sz - heap): %u\n",
                      (uint32)sections_total, (uint32)expected_sections_total);
            printfRed("  Total memory size: %u, Heap size: %u\n",
                      (uint32)total_memory_size, (uint32)heap_size);
            panic("verify_program_section_count fail");
            return false;
        }

        return true;
    }

    /****************************************************************************************
     * 堆内存管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::init_heap(uint64 start_addr)
    {
        // 设置ProcessMemoryManager中的堆地址
        heap_start = start_addr;
        heap_end = start_addr;
        reset_mmap_cursor(start_addr + k_mmap_guard_gap);

        printfGreen("ProcessMemoryManager: heap initialized successfully\n");
    }

    void ProcessMemoryManager::reset_mmap_cursor(uint64 minimum_start)
    {
        uint64 next_addr = PGROUNDUP(minimum_start);
        if (next_addr < k_mmap_min_base)
        {
            next_addr = k_mmap_min_base;
        }
        if (mmap_cursor < next_addr)
        {
            mmap_cursor = next_addr;
        }
    }

    bool ProcessMemoryManager::range_overlaps_used_vma(uint64 start_addr, uint64 end_addr) const
    {
        for (int i = 0; i < NVMA; ++i)
        {
            const vma &vm_entry = vma_data._vm[i];
            if (!vm_entry.used)
            {
                continue;
            }

            uint64 vma_start = vm_entry.addr;
            uint64 vma_end = vma_start + vm_entry.len;
            if (start_addr < vma_end && end_addr > vma_start)
            {
                return true;
            }
        }
        return false;
    }

    uint64 ProcessMemoryManager::reserve_mmap_region(uint64 size, uint64 alignment)
    {
        if (size == 0)
        {
            return 0;
        }

        uint64 aligned_size = PGROUNDUP(size);
        if (alignment < PGSIZE)
        {
            alignment = PGSIZE;
        }

        uint64 minimum_start = heap_end + k_mmap_guard_gap;
        if (minimum_start < heap_end)
        {
            return 0;
        }
        reset_mmap_cursor(minimum_start);

        uint64 upper_bound = TRAPFRAME;
        if (upper_bound > k_mmap_upper_guard)
        {
            upper_bound -= k_mmap_upper_guard;
        }
        upper_bound = PGROUNDDOWN(upper_bound);

        uint64 candidate = align_up_with_granularity(mmap_cursor, alignment);
        while (candidate < upper_bound)
        {
            uint64 candidate_end = candidate + aligned_size;
            if (candidate_end < candidate || candidate_end > upper_bound)
            {
                break;
            }

            bool conflict = range_overlaps_used_vma(candidate, candidate_end);

            if (!conflict)
            {
                for (int i = 0; i < prog_section_count; ++i)
                {
                    uint64 sec_start = PGROUNDDOWN((uint64)prog_sections[i]._sec_start);
                    uint64 sec_end = PGROUNDUP((uint64)prog_sections[i]._sec_start + prog_sections[i]._sec_size);
                    if (candidate < sec_end && candidate_end > sec_start)
                    {
                        conflict = true;
                        candidate = align_up_with_granularity(sec_end + PGSIZE, alignment);
                        break;
                    }
                }
            }

            if (!conflict)
            {
                mmap_cursor = candidate_end;
                return candidate;
            }

            if (candidate < minimum_start)
            {
                candidate = minimum_start;
            }
            candidate = align_up_with_granularity(candidate + PGSIZE, alignment);
        }

        printfRed("ProcessMemoryManager: no available mmap region for size=%p\n", (void *)aligned_size);
        return 0;
    }

    uint64 ProcessMemoryManager::grow_heap(uint64 new_end)
    {
        // 直接使用ProcessMemoryManager中的堆地址
        uint64 current_end = heap_end;
        if (new_end <= current_end)
        {
            return current_end; // 无需扩展
        }

        uint64 heap_limit = TRAPFRAME;
        if (heap_limit > k_mmap_guard_gap)
        {
            heap_limit -= k_mmap_guard_gap;
        }
        if (new_end >= heap_limit)
        {
            printfRed("ProcessMemoryManager: heap grow exceeds user space limit, new_end=%p\n", (void *)new_end);
            return current_end;
        }

        auto vma_covering_page = [&](uint64 page_va) -> const vma *
        {
            for (int i = 0; i < NVMA; ++i)
            {
                const vma &vm_entry = vma_data._vm[i];
                if (!vm_entry.used)
                {
                    continue;
                }

                uint64 vma_start = PGROUNDDOWN(vm_entry.addr);
                uint64 vma_end = PGROUNDUP(vm_entry.addr + vm_entry.len);
                if (page_va >= vma_start && page_va < vma_end)
                {
                    return &vm_entry;
                }
            }
            return nullptr;
        };

        auto map_heap_page = [&](uint64 page_va) -> bool
        {
            if (is_page_mapped(page_va))
            {
                return true;
            }

            void *mem = mem::PhysicalMemoryManager::alloc_page();
            if (mem == nullptr)
            {
                return false;
            }
            mem::k_pmm.clear_page(mem);

#ifdef RISCV
            uint64 flags = PTE_W | PTE_R | PTE_U;
#elif defined(LOONGARCH)
            uint64 flags = PTE_P | PTE_R | PTE_W | PTE_U | PTE_MAT | PTE_D;
#endif
            if (!mem::k_vmm.map_pages(pagetable, page_va, PGSIZE, (uint64)mem, flags))
            {
                mem::k_pmm.free_page(mem);
                return false;
            }
            return true;
        };

        uint64 first_page = PGROUNDDOWN(current_end);
        if (first_page < current_end)
        {
            first_page += PGSIZE;
        }
        auto rollback_heap_pages = [&](uint64 rollback_end)
        {
            for (uint64 rollback_va = first_page; rollback_va < rollback_end; rollback_va += PGSIZE)
            {
                if (vma_covering_page(rollback_va) != nullptr)
                {
                    continue;
                }
                if (is_page_mapped(rollback_va))
                {
                    mem::k_vmm.vmunmap(pagetable, rollback_va, 1, true);
                }
            }
        };

        for (uint64 va = first_page; va < PGROUNDUP(new_end); va += PGSIZE)
        {
            const vma *covering_vm = vma_covering_page(va);
            if (covering_vm != nullptr)
            {
                if (covering_vm->backing_kind == VMA_BACKING_SHM)
                {
                    printfRed("ProcessMemoryManager: heap grow would cross shared VMA [%p, %p)\n",
                              (void *)covering_vm->addr,
                              (void *)(covering_vm->addr + (uint64)covering_vm->len));
                    rollback_heap_pages(va);
                    return current_end;
                }
                // brk 可以把“堆顶”推进到已有 MAP_FIXED 私有映射之后；这类页仍归 VMA 管，
                // 堆只更新边界，不抢占页表映射。mmapstress03 专门覆盖这种带洞 brk 形态。
                // SysV SHM 共享映射已在上面拦截，shmt09 要求 brk 不能越过它。
                continue;
            }

            if (!map_heap_page(va))
            {
                printfRed("ProcessMemoryManager: heap grow failed at page %p\n", (void *)va);
                // 内核栈只有 8KB，不能在这里维护一个“已分配页数组”。失败时重新遍历
                // 本次扩展过的区间，释放非 VMA 覆盖的 heap 页即可。
                rollback_heap_pages(va);
                return current_end;
            }
        }

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;
        reset_mmap_cursor(heap_end + k_mmap_guard_gap);

        printfGreen("ProcessMemoryManager: heap grown successfully to %p\n", (void *)new_end);
        return new_end;
    }

    uint64 ProcessMemoryManager::shrink_heap(uint64 new_end)
    {
        // 直接使用ProcessMemoryManager中的堆地址
        uint64 current_end = heap_end;
        uint64 current_start = heap_start;

        if (new_end >= current_end || new_end < current_start)
        {
            return current_end; // 无效的收缩请求
        }

        // 释放多余的堆内存
        uint64 va_start = PGROUNDUP(new_end);
        uint64 va_end = PGROUNDUP(current_end);

        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            if (is_page_mapped(va))
            {
                // 检查是否为共享内存地址
                void* shm_start_addr = nullptr;
                size_t shm_size = 0;
                if (find_shared_backed_vma_covering(vma_data, va, &shm_start_addr, &shm_size))
                {
                    printfRed("[shrink_heap] heap range unexpectedly overlaps shared mapping at %p\n",
                              shm_start_addr);
                    return current_end;
                }
                else
                {
                    // 对于普通内存，直接使用vmunmap
                    mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                }
            }
        }

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;

        printfGreen("ProcessMemoryManager: heap shrunk successfully to %p\n", (void *)new_end);
        return new_end;
    }

    bool ProcessMemoryManager::cleanup_heap_to_size(uint64 new_size)
    {
        // 直接使用ProcessMemoryManager中的堆大小
        uint64 current_size = heap_end > heap_start ? heap_end - heap_start : 0;
        if (new_size >= current_size)
        {
            return true; // 无需收缩
        }

        uint64 new_end = heap_start + new_size;
        uint64 result = shrink_heap(new_end);

        return (result == new_end);
    }

    void ProcessMemoryManager::free_heap_memory()
    {
        // 重构：使用cleanup_heap_to_size(0)来完全释放堆内存
        cleanup_heap_to_size(0);
    }

    /****************************************************************************************
     * VMA管理接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_single_vma(int vma_index)
    {
        if (vma_index < 0 || vma_index >= NVMA || !vma_data._vm[vma_index].used)
        {
            return;
        }

        vma &vm_entry = vma_data._vm[vma_index];

        if (!is_reasonable_user_vma(vm_entry))
        {
            printfRed("ProcessMemoryManager: VMA %d 元数据异常，跳过页表释放并直接丢弃该条目\n",
                      vma_index);
            reset_vma_entry(vm_entry);
            return;
        }

        if (vm_entry.vfile != nullptr && !is_probably_kernel_object_ptr(vm_entry.vfile))
        {
            printfRed("ProcessMemoryManager: VMA %d 挂着异常文件指针 %p，跳过文件写回与 free_file\n",
                      vma_index, vm_entry.vfile);
            vm_entry.vfile = nullptr;
        }

        // printfBlue("  Processing VMA %d: addr=%p, len=%u, vfd=%d, flags=0x%x, prot=0x%x\n",
        //            vma_index, (void *)vm_entry.addr, vm_entry.len,
        //            vm_entry.vfd, vm_entry.flags, vm_entry.prot);

        // 退出路径只负责撤销映射与回收引用；显式 msync/munmap 已覆盖文件写回。
        // 这里继续碰可能已经悬空的 vfile，收益远小于把内核直接打死的风险。

        // 释放文件引用
        if (vm_entry.vfile != nullptr)
        {
            if (!is_probably_live_file_object(vm_entry.vfile))
            {
                printfRed("[free_single_vma] 检测到异常 vfile 指针，直接丢弃: vma=%d file=%p\n",
                          vma_index, vm_entry.vfile);
                vm_entry.vfile = nullptr;
            }
            else
            {
                vm_entry.vfile->free_file();
                vm_entry.vfile = nullptr;
            }
        }

        if (is_shared_backed_vma(vm_entry))
        {
            release_shared_backed_vma(*this, vma_index, vm_entry, true, "free_single_vma");
        }
        else
        {
            uint64 va_start = PGROUNDDOWN(vm_entry.addr);
            uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);
            if (vm_entry.has_resident_pages)
            {
                safe_vmunmap(va_start, va_end, true);
            }
        }

        reset_vma_entry(vm_entry);
    }

    void ProcessMemoryManager::free_all_vma()
    {
        // 遍历所有VMA条目，统一释放
        for (int i = 0; i < NVMA; ++i)
        {
            if (vma_data._vm[i].used)
            {
                free_single_vma(i);
            }
        }

        // printfGreen("ProcessMemoryManager: all VMA freed successfully\n");
    }

    int ProcessMemoryManager::unmap_memory_range(void *addr, size_t length)
    {
        if (!addr || length == 0)
        {
            return -1;
        }

        // 检查地址对齐
        if ((uint64)addr % PGSIZE != 0)
        {
            printfRed("ProcessMemoryManager: unmap address not page aligned: %p\n", addr);
            return -1;
        }

        uint64 start_addr = (uint64)addr;
        uint64 aligned_length = PGROUNDUP(length);
        uint64 end_addr = start_addr + aligned_length;

        // 检查地址范围溢出
        if (end_addr < start_addr)
        {
            printfRed("ProcessMemoryManager: address range overflow\n");
            return -1;
        }

        // printfYellow("ProcessMemoryManager: unmapping range [%p, %p) length=%u\n",
        //              addr, (void *)end_addr, aligned_length);

        // 查找重叠的VMA
        int overlapping_vmas[NVMA];
        int overlap_count = find_overlapping_vmas(start_addr, end_addr, overlapping_vmas, NVMA);

        if (overlap_count == 0)
        {
            printfYellow("ProcessMemoryManager: no VMA found for unmapping range\n");
            // 仍然尝试取消页表映射，以防有非VMA管理的映射
            safe_vmunmap(start_addr, end_addr, true);
            return 0;
        }

        // 处理每个重叠的VMA
        for (int i = 0; i < overlap_count; i++)
        {
            int vma_idx = overlapping_vmas[i];
            vma &vm_entry = vma_data._vm[vma_idx];

            uint64 vma_start = vm_entry.addr;
            uint64 vma_end = vm_entry.addr + vm_entry.len;

            printfCyan("ProcessMemoryManager: processing overlapping VMA %d: [%p, %p)\n",
                       vma_idx, (void *)vma_start, (void *)vma_end);

            // 计算需要取消映射的区域
            uint64 unmap_start = start_addr > vma_start ? start_addr : vma_start;
            uint64 unmap_end = end_addr < vma_end ? end_addr : vma_end;

            // 如果需要写回文件映射
            if (vm_entry.vfile != nullptr &&
                (vm_entry.flags & MAP_SHARED) &&
                (vm_entry.prot & PROT_WRITE) != 0)
            {
                printfCyan("ProcessMemoryManager: writing back shared file mapping\n");
                if (!writeback_file_mapping(vm_entry))
                {
                    return -1;
                }
            }
            bool full_unmap = (unmap_start == vma_start && unmap_end == vma_end);
            if (full_unmap)
            {
                if (is_shared_backed_vma(vm_entry))
                {
                    int detach_result = release_shared_backed_vma(*this, vma_idx, vm_entry, true, "munmap");
                    if (detach_result != 0)
                    {
                        return -1;
                    }
                }
                else
                {
                    if (vm_entry.has_resident_pages)
                    {
                        safe_vmunmap(unmap_start, unmap_end, true);
                    }
                }
            }
            else
            {
                if (vm_entry.has_resident_pages)
                {
                    safe_vmunmap(unmap_start, unmap_end, true);
                }
            }
            // 处理VMA条目的更新
            if (full_unmap)
            {
                // 完全取消映射
                // printfCyan("ProcessMemoryManager: completely unmapping VMA %d\n", vma_idx);
                if (vm_entry.vfile)
                {
                    if (!is_probably_live_file_object(vm_entry.vfile))
                    {
                        printfRed("ProcessMemoryManager: VMA %d 的 vfile 指针异常，直接丢弃: %p\n",
                                  vma_idx, vm_entry.vfile);
                    }
                    else
                    {
                        vm_entry.vfile->free_file();
                    }
                }
                reset_vma_entry(vm_entry);
            }
            else
            {
                // 部分取消映射
                if (!partial_unmap_vma(vma_idx, unmap_start, unmap_end))
                {
                    printfRed("ProcessMemoryManager: partial unmap failed for VMA %d\n", vma_idx);
                    return -1;
                }
            }
        }

        return 0;
    }

    bool ProcessMemoryManager::register_shared_attachment_vma(uint64 addr,
                                                              size_t length,
                                                              int prot,
                                                              int flags,
                                                              int shmid,
                                                              uint64 backing_base)
    {
        uint64 aligned_addr = PGROUNDDOWN(addr);
        uint64 aligned_len = PGROUNDUP(length);
        if (aligned_addr == 0 || aligned_len == 0)
        {
            printfRed("ProcessMemoryManager: register_shared_attachment_vma 参数非法 addr=%p len=%p\n",
                      (void *)addr, (void *)length);
            return false;
        }

        uint64 end_addr = aligned_addr + aligned_len;
        if (end_addr < aligned_addr || end_addr > MAXVA)
        {
            printfRed("ProcessMemoryManager: register_shared_attachment_vma 地址范围非法 [%p, %p)\n",
                      (void *)aligned_addr, (void *)end_addr);
            return false;
        }

        int free_idx = -1;
        for (int i = 0; i < NVMA; ++i)
        {
            vma &entry = vma_data._vm[i];
            if (!entry.used)
            {
                if (free_idx < 0)
                {
                    free_idx = i;
                }
                continue;
            }

            uint64 vm_start = entry.addr;
            uint64 vm_end = entry.addr + (uint64)entry.len;
            if (vm_end <= vm_start)
            {
                continue;
            }

            if (aligned_addr < vm_end && end_addr > vm_start)
            {
                printfRed("ProcessMemoryManager: 共享附件 VMA 与现有 VMA 冲突 idx=%d [%p, %p) vs [%p, %p)\n",
                          i,
                          (void *)aligned_addr,
                          (void *)end_addr,
                          (void *)vm_start,
                          (void *)vm_end);
                return false;
            }
        }

        if (free_idx < 0)
        {
            printfRed("ProcessMemoryManager: 没有空闲 VMA 槽位可登记共享附件 addr=%p len=%p\n",
                      (void *)aligned_addr, (void *)aligned_len);
            return false;
        }

        vma &vm = vma_data._vm[free_idx];
        reset_vma_entry(vm);
        vm.used = 1;
        vm.addr = aligned_addr;
        vm.len = static_cast<int>(aligned_len);
        vm.prot = prot;
        vm.flags = flags;
        vm.vfd = -1;
        vm.vfile = nullptr;
        vm.offset = 0;
        vm.max_len = aligned_len;
        vm.is_expandable = false;
        vm.backing_kind = VMA_BACKING_SHM;
        vm.backing_shmid = shmid;
        vm.backing_base = backing_base;
        return true;
    }

    int ProcessMemoryManager::clear_shared_attachment_vmas(int shmid, uint64 backing_base)
    {
        int cleared = 0;
        for (int i = 0; i < NVMA; ++i)
        {
            vma &entry = vma_data._vm[i];
            if (!entry.used || entry.backing_kind != VMA_BACKING_SHM)
            {
                continue;
            }
            if (entry.backing_shmid != shmid || entry.backing_base != backing_base)
            {
                continue;
            }

            reset_vma_entry(entry);
            ++cleared;
        }
        return cleared;
    }
    /// @brief 不修改堆顶的unmap
    /// @param addr 
    /// @param length 
    /// @return 
    int ProcessMemoryManager::unmap_memory_range_fix(void *addr, size_t length)
    {
        return unmap_memory_range(addr, length);
    }
    int ProcessMemoryManager::find_overlapping_vmas(uint64 start_addr, uint64 end_addr,
                                                    int overlapping_vmas[], int max_count)
    {
        if (!overlapping_vmas)
        {
            return 0;
        }

        int count = 0;
        for (int i = 0; i < NVMA && count < max_count; i++)
        {
            if (vma_data._vm[i].used)
            {
                uint64 vma_start = vma_data._vm[i].addr;
                uint64 vma_end = vma_start + vma_data._vm[i].len;

                // 检查是否有重叠
                if (start_addr < vma_end && end_addr > vma_start)
                {
                    overlapping_vmas[count++] = i;
                }
            }
        }

        return count;
    }

    bool ProcessMemoryManager::partial_unmap_vma(int vma_index, uint64 unmap_start, uint64 unmap_end)
    {
        if (!is_vma_valid(vma_index))
        {
            return false;
        }

        vma &vm_entry = vma_data._vm[vma_index];
        uint64 vma_start = vm_entry.addr;
        uint64 vma_end = vm_entry.addr + vm_entry.len;

        if (unmap_start == vma_start && unmap_end < vma_end)
        {
            // 从VMA开始处取消映射
            // printfCyan("ProcessMemoryManager: unmapping from start of VMA %d\n", vma_index);
            vm_entry.addr = unmap_end;
            vm_entry.len = vma_end - unmap_end;
            if (vm_entry.vfile)
            {
                vm_entry.offset += (unmap_end - vma_start);
            }
            return true;
        }
        else if (unmap_start > vma_start && unmap_end == vma_end)
        {
            // 从VMA末尾取消映射
            // printfCyan("ProcessMemoryManager: unmapping from end of VMA %d\n", vma_index);
            vm_entry.len = unmap_start - vma_start;
            return true;
        }
        else if (unmap_start > vma_start && unmap_end < vma_end)
        {
            // 从VMA中间取消映射（需要分割VMA）
            printfCyan("ProcessMemoryManager: implementing middle unmapping (VMA split)\n");
            
            // 找到一个空的VMA槽位用于分割后的后半部分
            int new_vma_idx = -1;
            for (int j = 0; j < NVMA; j++)
            {
                if (!vma_data._vm[j].used)
                {
                    new_vma_idx = j;
                    break;
                }
            }
            
            if (new_vma_idx == -1)
            {
                printfRed("ProcessMemoryManager: no free VMA slot for split\n");
                return false;
            }
            
            // 创建后半部分的VMA，保留同一后端元数据，避免 split 后生命周期丢失
            vma &new_vm = vma_data._vm[new_vma_idx];
            new_vm = vm_entry;
            new_vm.used = 1;
            new_vm.addr = unmap_end;
            new_vm.len = vma_end - unmap_end;
            if (vm_entry.vfile)
            {
                new_vm.offset = vm_entry.offset + (unmap_end - vma_start);
                vm_entry.vfile->dup();
            }
            else
            {
                new_vm.offset = 0;
            }
            
            // 修改原VMA为前半部分
            vm_entry.len = unmap_start - vma_start;
            
            printfCyan("ProcessMemoryManager: split VMA %d into [%p, %p) and VMA %d [%p, %p)\n",
                       vma_index, (void *)vm_entry.addr, (void *)(vm_entry.addr + vm_entry.len),
                       new_vma_idx, (void *)new_vm.addr, (void *)(new_vm.addr + new_vm.len));
            
            return true;
        }

        return false;
    }

    /****************************************************************************************
     * 页表管理接口实现
     ****************************************************************************************/

    bool ProcessMemoryManager::create_pagetable()
    {
        // 创建基础页表
        mem::PageTable pt = mem::k_vmm.vm_create();
        if (pt.is_null() || pt.get_base() == 0)
        {
            printfRed("ProcessMemoryManager: vm_create failed\n");
            return false;
        }

#ifdef RISCV
        // 映射trampoline页面
        if (mem::k_vmm.map_pages(pt, TRAMPOLINE, PGSIZE, (uint64)trampoline,
                                 riscv::PteEnum::pte_readable_m | riscv::pte_executable_m) == 0)
        {
            panic("ProcessMemoryManager: map trampoline failed\n");
            pt.freewalk();
            return false;
        }

        // 注意：trapframe映射延迟到usertrapret时进行

        // 映射信号trampoline页面
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline,
                                 riscv::PteEnum::pte_readable_m | riscv::pte_executable_m | riscv::PteEnum::pte_user_m) == 0)
        {
            panic("ProcessMemoryManager: map sigtrapframe failed\n");
            // 先取消已成功的映射，再释放页表
            mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
            pt.freewalk();
            return false;
        }

#elif defined(LOONGARCH)
        // 注意：trapframe映射延迟到usertrapret时进行

        // 映射信号trampoline页面
        if (mem::k_vmm.map_pages(pt, SIG_TRAMPOLINE, PGSIZE, (uint64)sig_trampoline,
                                 PTE_P | PTE_MAT | PTE_D | PTE_U) == 0)
        {
            panic("ProcessMemoryManager: Fail to map sig_trampoline\n");
            pt.freewalk();
            return false;
        }
#endif

        // 设置页表
        pagetable = pt;
        return true;
    }

    void ProcessMemoryManager::free_pagetable()
    {
        if (!pagetable.get_base())
        {
            printfYellow("ProcessMemoryManager: pagetable already released, skip free_pagetable\n");
            return;
        }

        mem::PageTable &pt = pagetable;

        // 阶段1：不再依赖分散的引用计数，直接释放
        // 取消特殊页面的映射
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0);
#endif
        mem::k_vmm.vmunmap(pt, TRAPFRAME, 1, 0); // 有可能没有映射
        mem::k_vmm.vmunmap(pt, SIG_TRAMPOLINE, 1, 0);

        pt.freewalk();
        pagetable.set_base(0);

        printfGreen("ProcessMemoryManager: pagetable freed successfully\n");
    }

    void ProcessMemoryManager::safe_vmunmap(uint64 va_start, uint64 va_end, bool check_validity)
    {
        if (!pagetable.get_base())
        {
            return;
        }

        // 确保地址对齐到页边界
        va_start = PGROUNDDOWN(va_start);
        va_end = PGROUNDUP(va_end);

        // 用户态普通映射绝不能触碰 trapframe / signal trampoline / trampoline 这段保留区。
        // 如果上层把长度、VMA 或堆边界算错了，这里至少要把错误限制在普通用户区，
        // 不能因为一次错误的 munmap 把整个进程返回用户态所依赖的固定映射拆掉。
        if (va_start >= TRAPFRAME)
        {
            printf("\33[1;31mProcessMemoryManager: safe_vmunmap 请求进入保留区，已拒绝 start=%p end=%p\33[0m\n",
                   (void *)va_start, (void *)va_end);
            return;
        }
        if (va_end > TRAPFRAME)
        {
            printf("\33[1;31mProcessMemoryManager: safe_vmunmap 请求跨越保留区，自动截断 start=%p end=%p -> %p\33[0m\n",
                   (void *)va_start, (void *)va_end, (void *)TRAPFRAME);
            va_end = TRAPFRAME;
        }
        if (va_start >= va_end)
        {
            return;
        }

        for (uint64 va = va_start; va < va_end; va += PGSIZE)
        {
            if (check_validity)
            {
                mem::Pte pte = pagetable.walk(va, 0);
                if (!pte.is_null() && pte.is_valid())
                {
                    // 检查是否为共享内存地址
                    void* shm_start_addr = nullptr;
                    size_t shm_size = 0;
                    bool is_shared_page = find_shared_backed_vma_covering(vma_data, va, &shm_start_addr, &shm_size);
                    // printfBlue("[safe_vmunmap] Attempting to unmap VA=%p,tid:%d\n", (void *)va,shmid);
                    if (is_shared_page)
                    {
                        mem::k_vmm.vmunmap(pagetable, va, 1, 0);
                    }
                    else
                    {
                        // 对于普通内存，直接使用vmunmap
                        mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                    }
                }
            }
            else
            {
                // 不检查有效性时也要区分共享内存和普通内存
                void* shm_start_addr = nullptr;
                size_t shm_size = 0;
                if (find_shared_backed_vma_covering(vma_data, va, &shm_start_addr, &shm_size))
                {
                    mem::k_vmm.vmunmap(pagetable, va, 1, 0);
                }
                else
                {
                    // 对于普通内存，直接尝试取消映射
                    mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                }
            }
        }
    }

    /****************************************************************************************
     * 统一内存释放接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::free_all_memory()
    {
        // 减少引用计数，只有当引用计数降为0时才释放整个内存
        int old_count = ref_count.fetch_sub(1, eastl::memory_order_acq_rel);
        
        if (old_count <= 1)
        {
            // 线程共享地址空间路径里，如果引用计数已经漂掉，但进程池里仍有其他 PCB
            // 指向当前 mm，就绝不能继续 free 页表，否则会把仍在运行的线程直接打死。
            int holders = count_live_mm_holders(this);
            if (holders > 1)
            {
                int remaining_holders = holders - 1;
                ref_count.store(remaining_holders, eastl::memory_order_release);
                shared_vm = true;
                printfYellow("ProcessMemoryManager: refcount drift repaired, mm=%p holders=%d remaining=%d\n",
                             this, holders, remaining_holders);
                return;
            }

            // 引用计数降为0，释放所有内存资源
            // print_memory_usage();
            // 1. 释放VMA
            free_all_vma();
            // printfGreen("ProcessMemoryManager: all VMA freed\n");
            shared_vm = false;

            // 2. 如果页表存在，释放程序段和堆内存
            if (pagetable.get_base())
            {
                free_all_program_sections();
                // printfGreen("ProcessMemoryManager: all program sections freed\n");
                free_heap_memory();
                // printfGreen("ProcessMemoryManager: heap memory freed\n");
                free_pagetable();
                // printfGreen("ProcessMemoryManager: pagetable freed\n");
            }
            else
            {
                printfYellow("ProcessMemoryManager: pagetable already null during free_all_memory\n");
            }

            // 3. 重置内存相关状态
            reset_memory_sections();
        }
        else
        {
            shared_vm = true;
        }
        // 如果引用计数还大于0，说明还有其他进程/线程在使用这块内存，不进行释放
    }

    void ProcessMemoryManager::emergency_cleanup()
    {
        printfRed("ProcessMemoryManager: emergency cleanup\n");

        // 紧急清理：不进行写回操作，只释放内存

        // 1. 强制释放VMA（不写回）
        for (int i = 0; i < NVMA; ++i)
        {
            if (vma_data._vm[i].used)
            {
                // 只释放文件引用，不写回
                if (vma_data._vm[i].vfile != nullptr)
                {
                    if (!is_probably_live_file_object(vma_data._vm[i].vfile))
                    {
                        printfRed("ProcessMemoryManager: emergency cleanup 发现异常 vfile 指针，直接丢弃: vma=%d file=%p\n",
                                  i, vma_data._vm[i].vfile);
                    }
                    else
                    {
                        vma_data._vm[i].vfile->free_file();
                    }
                }

                // 取消映射
                uint64 va_start = PGROUNDDOWN(vma_data._vm[i].addr);
                uint64 va_end = PGROUNDUP(vma_data._vm[i].addr + vma_data._vm[i].len);
                if (is_shared_backed_vma(vma_data._vm[i]))
                {
                    release_shared_backed_vma(*this, i, vma_data._vm[i], false, "emergency_cleanup");
                }
                else
                {
                    safe_vmunmap(va_start, va_end, false); // 不检查有效性
                }

                reset_vma_entry(vma_data._vm[i]);
            }
        }
        shared_vm = false;

        // 2. 释放其他内存资源
        if (pagetable.get_base())
        {
            free_all_program_sections();
            free_heap_memory();
            free_pagetable();
        }

        reset_memory_sections();

        printfRed("ProcessMemoryManager: emergency cleanup completed\n");
    }

    void ProcessMemoryManager::cleanup_execve_pagetable(mem::PageTable &pagetable,
                                                        const program_section_desc *section_descs,
                                                        int section_count)
    {
        if (!pagetable.get_base())
        {
            printfYellow("cleanup_execve_pagetable: invalid pagetable, skipping cleanup\n");
            return;
        }

        printfRed("cleanup_execve_pagetable: cleaning up %d allocated sections\n", section_count);

        // 遍历所有已记录的程序段，释放其占用的内存
        for (int i = 0; i < section_count; i++)
        {
            if (section_descs[i]._sec_start && section_descs[i]._sec_size > 0)
            {
                uint64 va_start = PGROUNDDOWN((uint64)section_descs[i]._sec_start);
                uint64 va_end = PGROUNDUP((uint64)section_descs[i]._sec_start + section_descs[i]._sec_size);

                printfRed("  Cleaning section %d (%s): %p - %p (%u bytes)\n",
                          i,
                          section_descs[i]._debug_name ? section_descs[i]._debug_name : "unnamed",
                          (void *)va_start,
                          (void *)va_end,
                          section_descs[i]._sec_size);

                // 直接使用vmunmap清理，不检查页面有效性以提高错误处理的鲁棒性
                for (uint64 va = va_start; va < va_end; va += PGSIZE)
                {
                    mem::k_vmm.vmunmap(pagetable, va, 1, 1);
                }
            }
        }

        // 清理页表的特殊映射（trampoline、sig_trampoline等）
#ifdef RISCV
        mem::k_vmm.vmunmap(pagetable, TRAMPOLINE, 1, 0);
#endif
        // 注意：trapframe映射由usertrapret管理，这里不需要显式取消映射
        mem::k_vmm.vmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);

        // 阶段1：不再使用分散的引用计数
        // pagetable.dec_ref(); // 注释掉分散的引用计数操作

        printfGreen("cleanup_execve_pagetable: cleanup completed\n");
    }

    /****************************************************************************************
     * 内存调试和监控接口实现
     ****************************************************************************************/

    void ProcessMemoryManager::update_total_memory_size()
    {
        total_memory_size = calculate_total_memory_size();
    }

    uint64 ProcessMemoryManager::calculate_total_memory_size() const
    {
        uint64 total = 0;

        // 计算所有程序段的大小（与get_total_program_memory()逻辑相同）
        for (int i = 0; i < prog_section_count; i++)
        {
            total += prog_sections[i]._sec_size;
        }

        // 加上堆的大小
        if (heap_end > heap_start)
        {
            total += (heap_end - heap_start);
        }

        return total;
    }

    bool ProcessMemoryManager::verify_memory_consistency()
    {
        uint64 calculated_total = calculate_total_memory_size();
        bool consistent = (total_memory_size == calculated_total);

        if (!consistent)
        {
            printfRed("Memory inconsistency detected\n");
            printfRed("  total_memory_size: %u, calculated: %u\n", (uint32)total_memory_size, (uint32)calculated_total);
            printfRed("  Note: VMA regions are managed separately and not counted in total_memory_size\n");
            panic("ProcessMemoryManager verify_memory_consistency failed\n");
        }

        return consistent;
    }

    void ProcessMemoryManager::print_memory_usage() const
    {
        printfCyan("=== ProcessMemoryManager Memory Information ===\n");
        printfCyan("Total process size: %u bytes\n", (uint32)total_memory_size);

        // 程序段信息
        printfCyan("Program sections (%d):\n", prog_section_count);
        uint64 sections_total = 0;
        for (int i = 0; i < prog_section_count; i++)
        {
            printfCyan("  Section %d (%s): %p - %p (%u bytes)\n",
                       i,
                       prog_sections[i]._debug_name ? prog_sections[i]._debug_name : "unnamed",
                       prog_sections[i]._sec_start,
                       (void *)((uint64)prog_sections[i]._sec_start + prog_sections[i]._sec_size),
                       (uint32)prog_sections[i]._sec_size);
            sections_total += prog_sections[i]._sec_size;
        }
        printfCyan("Total program sections: %u bytes\n", (uint32)sections_total);

        // 堆信息
        uint64 heap_size = (heap_end > heap_start) ? (heap_end - heap_start) : 0;
        if (heap_size > 0)
        {
            printfCyan("Heap: %p - %p (%u bytes)\n",
                       (void *)heap_start,
                       (void *)heap_end,
                       (uint32)heap_size);
        }
        else
        {
            printfCyan("Heap: not allocated\n");
        }

        // VMA信息
        printfCyan("VMA structure: present\n");
        uint64 vma_total = 0;
        int active_vmas = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                printfCyan("  VMA %d: %p - %p (%u bytes, prot=%d, flags=%d)\n",
                           i,
                           (void *)vma_data._vm[i].addr,
                           (void *)(vma_data._vm[i].addr + vma_data._vm[i].len),
                           (uint32)vma_data._vm[i].len,
                           vma_data._vm[i].prot,
                           vma_data._vm[i].flags);
                vma_total += vma_data._vm[i].len;
                active_vmas++;
            }
        }
        printfCyan("Total VMA usage: %u bytes (%d active VMAs)\n", (uint32)vma_total, active_vmas);

        // 页表信息
        if (pagetable.get_base())
        {
            printfCyan("Page table: present (%p)\n", pagetable.get_base());
        }
        else
        {
            printfCyan("Page table: not present\n");
        }

        printfCyan("=== End ProcessMemoryManager Memory Information ===\n");
    }

    bool ProcessMemoryManager::verify_all_memory_consistency() const
    {
        bool consistent = true;

        // 检查程序段一致性
        if (!verify_program_sections_consistency())
        {
            consistent = false;
        }

        // 检查总内存大小一致性（类似于verify_memory_consistency的逻辑）
        uint64 calculated_total = calculate_total_memory_size();
        if (total_memory_size != calculated_total)
        {
            printfRed("Memory inconsistency detected in verify_all_memory_consistency\n");
            printfRed("  total_memory_size: %u, calculated: %u\n",
                      (uint32)total_memory_size, (uint32)calculated_total);
            consistent = false;
        }

        return consistent;
    }

    uint64 ProcessMemoryManager::get_total_memory_usage() const
    {
        // 直接返回缓存的总内存大小，等价于calculate_total_memory_size()的结果
        return total_memory_size;
    }

    uint64 ProcessMemoryManager::get_vma_memory_usage() const
    {
        uint64 total = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                total += vma_data._vm[i].len;
            }
        }
        return total;
    }

    bool ProcessMemoryManager::check_memory_leaks() const
    {
        bool leaks_detected = false;

        // 检查是否有未释放的程序段
        if (prog_section_count > 0)
        {
            printfYellow("ProcessMemoryManager: %d program sections still present\n",
                         prog_section_count);
            leaks_detected = true;
        }

        // 检查是否有未释放的堆内存
        uint64 heap_size = (heap_end > heap_start) ? (heap_end - heap_start) : 0;
        if (heap_size > 0)
        {
            printfYellow("ProcessMemoryManager: heap memory still present (%u bytes)\n",
                         (uint32)heap_size);
            leaks_detected = true;
        }

        // 检查是否有未释放的VMA
        int active_vmas = 0;
        for (int i = 0; i < NVMA; i++)
        {
            if (vma_data._vm[i].used)
            {
                active_vmas++;
            }
        }
        if (active_vmas > 0)
        {
            printfYellow("ProcessMemoryManager: %d VMA entries still active\n", active_vmas);
            leaks_detected = true;
        }

        return leaks_detected;
    }

    /****************************************************************************************
     * 内部辅助函数实现
     ****************************************************************************************/

    bool ProcessMemoryManager::is_page_mapped(uint64 va)
    {
        if (!pagetable.get_base())
        {
            return false;
        }

        mem::Pte pte = pagetable.walk(va, 0);
        return !pte.is_null() && pte.is_valid();
    }



    bool ProcessMemoryManager::writeback_file_mapping(const vma &vma_entry)
    {
        if (vma_entry.vfile == nullptr)
        {
            return true; // 匿名映射，无需写回
        }

        if ((vma_entry.flags & MAP_SHARED) == 0 || (vma_entry.prot & PROT_WRITE) == 0)
        {
            return true; // 非共享或不可写，无需写回
        }

        const uint64 vma_start = vma_entry.addr;
        const uint64 vma_end = vma_entry.addr + vma_entry.len;
        const uint64 page_start = PGROUNDDOWN(vma_start);
        const uint64 page_end = PGROUNDUP(vma_end);
        uint64 file_size = 0;
        bool has_file_size = false;
        fs::Kstat st = {};
        if (vfs_fstat(vma_entry.vfile, &st) == 0)
        {
            file_size = st.size;
            has_file_size = true;
        }

        for (uint64 va = page_start; va < page_end; va += PGSIZE)
        {
            mem::Pte pte = pagetable.walk(va, 0);
            if (pte.is_null() || !pte.is_valid())
            {
                continue; // 惰性 mmap 未驻留页没有脏数据可写回
            }

            if (!pte_allows_user_access(pte))
            {
                printfRed("[ProcessMemoryManager] skip non-user file mapping page va=%p\n",
                          reinterpret_cast<void *>(va));
                return false;
            }

            uint64 write_start = va;
            if (write_start < vma_start)
            {
                write_start = vma_start;
            }
            uint64 write_end = va + PGSIZE;
            if (write_end > vma_end)
            {
                write_end = vma_end;
            }
            if (write_end <= write_start)
            {
                continue;
            }

            const size_t write_len = static_cast<size_t>(write_end - write_start);
            const uint64 page_offset = write_start - va;
            const uint64 file_offset = vma_entry.offset + (write_start - vma_start);
            if (has_file_size && file_offset >= file_size)
            {
                continue;
            }

            size_t bounded_write_len = write_len;
            if (has_file_size && file_offset + bounded_write_len > file_size)
            {
                // 文件尾页 EOF 之后的 MAP_SHARED 字节必须保持“内存可写、文件不可见”。
                // Linux 不会因为 msync/munmap 把这些尾部脏字节扩展进文件。
                bounded_write_len = static_cast<size_t>(file_size - file_offset);
            }
            const uint64 kernel_buf = pte_data_kernel_addr(pte) + page_offset;

            // file::write 只接受内核可直接访问的缓冲区。MAP_SHARED 写回必须
            // 逐页把用户 VA 转成页表里的真实物理页，不能把 VMA 地址当指针。
            long result = vma_entry.vfile->write(kernel_buf,
                                                 bounded_write_len,
                                                 static_cast<long>(file_offset),
                                                 false);
            if (result < 0 || static_cast<size_t>(result) != bounded_write_len)
            {
                printfRed("[ProcessMemoryManager] Failed to write back file mapping va=%p len=%zu off=%p result=%ld\n",
                          reinterpret_cast<void *>(write_start),
                          bounded_write_len,
                          file_offset,
                          result);
                return false;
            }
        }

        return true;
    }

    bool ProcessMemoryManager::is_vma_valid(int vma_index) const
    {
        if (vma_index < 0 || vma_index >= NVMA)
        {
            return false;
        }

        return vma_data._vm[vma_index].used;
    }

    uint64 ProcessMemoryManager::calculate_page_count(uint64 start_addr, uint64 size) const
    {
        uint64 start_aligned = PGROUNDDOWN(start_addr);
        uint64 end_aligned = PGROUNDUP(start_addr + size);
        return (end_aligned - start_aligned) / PGSIZE;
    }

    uint64 ProcessMemoryManager::align_to_page(uint64 addr, bool round_up) const
    {
        if (round_up)
        {
            return PGROUNDUP(addr);
        }
        else
        {
            return PGROUNDDOWN(addr);
        }
    }

} // namespace proc

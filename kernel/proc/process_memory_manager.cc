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
#include "vm_object.hh"
#include "hal/arch.hh"
#include "memlayout.hh"
#include "mem/kernel_image.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/vfs_utils.hh"
#include "fs/vfs/virtual_fs.hh"
#include "shm/shm_manager.hh"
#include "hal/tlb_shootdown.hh"
#include "vma_metadata_utils.hh"
#include "scheduler.hh"
#include <EASTL/vector.h>

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
        constexpr uint32 k_file_invalidation_registry_capacity = num_process;
        inline uint32 max_reasonable_file_refcnt()
        {
            return num_process * max_open_files;
        }

        struct FileInvalidationMmRegistry
        {
            FileInvalidationMmRegistry()
            {
                lock.init("file_mmap_registry");
            }

            SpinLock lock;
            ProcessMemoryManager *slots[k_file_invalidation_registry_capacity]{};
            uint64 generation = 1;
        };

        FileInvalidationMmRegistry g_file_invalidation_mm_registry;

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

        inline bool prefer_vm_space_lookup(const ProcessMemoryManager &mm, uint64 addr)
        {
            // 动态 mmap/线程栈都分配在 heap 之后的高地址区间，而这部分现在统一由
            // VMASpace 承载。低地址仍保留 legacy program/heap 镜像，因此可以按地址
            // 做一次轻量分流，避免每次都把两套索引同时扫一遍。
            uint64 dynamic_base = mm.heap_end;
            if (dynamic_base > UINT64_MAX - k_mmap_guard_gap)
            {
                dynamic_base = UINT64_MAX;
            }
            else
            {
                dynamic_base += k_mmap_guard_gap;
            }
            return addr >= dynamic_base;
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

        bool wipe_child_vma_pages(ProcessMemoryManager &child_mm, uint64 start, uint64 len)
        {
            if (len == 0)
            {
                return true;
            }

            uint64 begin = PGROUNDDOWN(start);
            uint64 end = PGROUNDUP(start + len);
            if (end < begin)
            {
                return false;
            }

            for (uint64 va = begin; va < end; va += PGSIZE)
            {
                mem::Pte pte = child_mm.pagetable.walk(va, false);
                if (pte.is_null() || !pte.is_valid())
                {
                    continue;
                }

                /*
                 * MADV_WIPEONFORK 同样允许作用于只读的私有匿名映射。fork
                 * 后可写页已经带 COW 位，而只读页只是普通共享只读 PTE；后者
                 * 也必须先临时标成 COW，才能让统一拆页路径正确更新对象 overlay
                 * 和物理页引用计数。清零完成后再恢复原来的逻辑权限，绝不能
                 * 因内核写零而把用户只读页意外提升成可写页。
                 */
                const uint64 original_data = pte.get_data();
#ifdef RISCV
                const bool logically_writable =
                    (original_data & (riscv::PteEnum::pte_writable_m |
                                      mem::k_riscv_pte_cow)) != 0;
                if ((original_data & mem::k_riscv_pte_cow) == 0)
                {
                    pte.set_data(original_data | mem::k_riscv_pte_cow);
                }
#elif defined(LOONGARCH)
                const bool logically_writable =
                    (original_data & (PTE_W | PTE_COW)) != 0;
                if ((original_data & PTE_COW) == 0)
                {
                    pte.set_data(original_data | PTE_COW);
                }
#endif

                if (mem::k_vmm.resolve_cow_page(child_mm.pagetable, va, &child_mm) != 0)
                {
                    return false;
                }
                pte = child_mm.pagetable.walk(va, false);
                if (pte.is_null() || !pte.is_valid())
                {
                    return false;
                }
                memset(user_page_kernel_ptr((uint64)pte.pa()), 0, PGSIZE);

#ifdef RISCV
                uint64 final_data = pte.get_data() & ~mem::k_riscv_pte_cow;
                if (logically_writable)
                    final_data |= riscv::PteEnum::pte_writable_m;
                else
                    final_data &= ~riscv::PteEnum::pte_writable_m;
                pte.set_data(final_data);
#elif defined(LOONGARCH)
                uint64 final_data = pte.get_data() & ~PTE_COW;
                if (logically_writable)
                    final_data |= PTE_W | PTE_D;
                else
                    final_data &= ~PTE_W;
                pte.set_data(final_data);
#endif
            }

            // PTE 地址没有改变，但权限可能从临时可写恢复为只读；返回父/子进程
            // 前必须让所有可能运行该 child mm 的 CPU 丢掉旧权限翻译。
            hal::tlb::flush_mm_range(child_mm, begin, end - begin);
            return true;
        }

        inline bool is_shared_backed_vma(const vma &entry)
        {
            return entry.used && entry.backing_kind == VMA_BACKING_SHM && entry.backing_shmid >= 0;
        }

        inline bool mapping_pages_should_be_freed_on_unmap(const vma &entry)
        {
            (void)entry;
            // 这里的 do_free 表示释放“当前页表映射引用”，不是释放共享后端的
            // owner 引用。VmObject::prepare_page()/fork remap 都会 retain 一份
            // mapping 引用；MAP_SHARED/SysV SHM 退出时同样必须 drop，否则对象析构
            // 只会释放 source owner，剩余 mapping 引用会让 PMM 页永久驻留。
            return true;
        }

        inline vma *pick_lower_addr_vma(vma *lhs, vma *rhs)
        {
            if (lhs == nullptr)
            {
                return rhs;
            }
            if (rhs == nullptr)
            {
                return lhs;
            }
            return lhs->addr <= rhs->addr ? lhs : rhs;
        }

        inline const vma *pick_lower_addr_vma(const vma *lhs, const vma *rhs)
        {
            if (lhs == nullptr)
            {
                return rhs;
            }
            if (rhs == nullptr)
            {
                return lhs;
            }
            return lhs->addr <= rhs->addr ? lhs : rhs;
        }

        inline vma *pick_higher_addr_vma(vma *lhs, vma *rhs)
        {
            if (lhs == nullptr)
            {
                return rhs;
            }
            if (rhs == nullptr)
            {
                return lhs;
            }
            return lhs->addr >= rhs->addr ? lhs : rhs;
        }

        inline const vma *pick_higher_addr_vma(const vma *lhs, const vma *rhs)
        {
            if (lhs == nullptr)
            {
                return rhs;
            }
            if (rhs == nullptr)
            {
                return lhs;
            }
            return lhs->addr >= rhs->addr ? lhs : rhs;
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

        bool remap_shared_object_vma(ProcessMemoryManager &target_mm,
                                     mem::PageTable &src_pt,
                                     const vma &entry)
        {
            if (entry.object == nullptr || !entry.object->shared_mapping())
            {
                return true;
            }

            uint64 map_start = PGROUNDDOWN(entry.addr);
            uint64 map_end = PGROUNDUP(entry.addr + static_cast<uint64>(entry.len));
            if (map_end < map_start)
            {
                return false;
            }

            for (uint64 va = map_start; va < map_end; va += PGSIZE)
            {
                mem::Pte src_pte = src_pt.walk(va, false);
                if (src_pte.is_null() || !src_pte.is_valid())
                {
                    continue;
                }

                uint64 pa = reinterpret_cast<uint64>(src_pte.pa());
                void *page = user_page_kernel_ptr(pa);
                if (mem::k_pmm.is_managed_page(page) && !mem::k_pmm.retain_page(page))
                {
                    return false;
                }

                if (!mem::k_vmm.map_pages(target_mm.pagetable, va, PGSIZE, pa, src_pte.get_flags()))
                {
                    if (mem::k_pmm.is_managed_page(page))
                    {
                        mem::k_pmm.free_page(page);
                    }
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

        inline bool has_other_shared_backing_fragment(const ProcessMemoryManager &mm,
                                                      const vma *skip_entry,
                                                      const vma &target)
        {
            for (int i = 0; i < NVMA; ++i)
            {
                const vma &entry = mm.vma_data._vm[i];
                if (&entry == skip_entry)
                {
                    continue;
                }
                if (is_same_shared_backing(entry, target))
                {
                    return true;
                }
            }

            bool found = false;
            mm.get_vm_space().for_each([&](const vma &entry) -> bool
            {
                if (&entry != skip_entry && is_same_shared_backing(entry, target))
                {
                    found = true;
                    return false;
                }
                return true;
            });
            return found;
        }

        inline uint64 segment_file_backed_bytes(const vma &source, uint64 segment_offset, uint64 segment_len)
        {
            if (source.file_backed_bytes == 0 || segment_len == 0)
            {
                return 0;
            }
            if (segment_offset >= source.file_backed_bytes)
            {
                return 0;
            }
            uint64 remaining = source.file_backed_bytes - segment_offset;
            return remaining > segment_len ? segment_len : remaining;
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
            if (start >= USER_MEMORY_TOP || end > USER_MEMORY_TOP)
            {
                return false;
            }
            return true;
        }

        inline bool is_probably_kernel_object_ptr(const void *ptr)
        {
            return (uint64)ptr >= mem::kernel_image_start_address();
        }

        inline bool is_kernel_mapped_range(uint64 addr, uint64 size)
        {
            if (addr < mem::kernel_image_start_address() || size == 0)
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
            return refcnt > 0 && refcnt <= max_reasonable_file_refcnt();
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
            if (start >= USER_MEMORY_TOP || end > USER_MEMORY_TOP)
            {
                return false;
            }
            return true;
        }

        inline int release_shared_backed_vma(ProcessMemoryManager &mm,
                                             const vma *skip_entry,
                                             const vma &vm_entry,
                                             bool check_validity,
                                             const char *context)
        {
            uint64 va_start = PGROUNDDOWN(vm_entry.addr);
            uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);

            // 共享后端仍持有 owner 引用；safe_vmunmap 释放的是当前页表映射引用。
            mm.safe_vmunmap(va_start, va_end, check_validity);

            // 同一条共享段经过 split/trim 之后可能散成多个 VMA 片段。
            // 只有最后一个片段离开时，才真正 detach 那条共享段附件记录。
            if (has_other_shared_backing_fragment(mm, skip_entry, vm_entry))
            {
                return 0;
            }

            Pcb *current = k_pm.get_cur_pcb();
            if (current == nullptr)
            {
                return -1;
            }

            int detach_result = shm::k_smm.detach_vma_attachment(vm_entry.backing_shmid,
                                                                 (void *)vm_entry.backing_base,
                                                                 current->get_tid());
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
        : prog_section_count(0), heap_start(0), heap_end(0), heap_high_watermark(0),
          mmap_cursor(0), shared_vm(false),
          total_memory_size(0), ref_count(1)
    {
        tlb_state_lock.init("mm_tlb_state");
        tlb_flush_lock.init("mm_tlb_flush");
        user_asid = allocate_user_asid();
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
        vma_index.clear();
        vm_space.init(this);
    }

    ProcessMemoryManager::~ProcessMemoryManager()
    {
        // 失败创建路径可能直接 delete；摘除操作是幂等的。
        retire_file_invalidation_registry();
        retire_user_asid(user_asid);
        user_asid = 0;
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
        if (old_count <= 0)
        {
            panic("ProcessMemoryManager::put underflow mm=%p old_refs=%d", this, old_count);
        }
        return old_count == 1;
    }

    int ProcessMemoryManager::get_ref_count() const
    {
        return ref_count.load(eastl::memory_order_acquire);
    }

    void ProcessMemoryManager::lock_memory()
    {
        if (memory_lock.is_holding())
        {
            ++memory_lock_depth;
            return;
        }
        memory_lock.acquire();
        memory_lock_depth = 1;
    }

    void ProcessMemoryManager::unlock_memory()
    {
        if (!memory_lock.is_holding() || memory_lock_depth == 0)
        {
            panic("ProcessMemoryManager::unlock_memory without ownership");
        }
        if (memory_lock_depth > 1)
        {
            --memory_lock_depth;
            return;
        }
        memory_lock_depth = 0;
        memory_lock.release();
    }

    void ProcessMemoryManager::publish_file_invalidation_registry()
    {
        FileInvalidationMmRegistry &registry = g_file_invalidation_mm_registry;
        registry.lock.acquire();
        if (file_invalidation_registered)
        {
            registry.lock.release();
            return;
        }

        for (uint32 index = 0; index < k_file_invalidation_registry_capacity; ++index)
        {
            if (registry.slots[index] != nullptr)
            {
                continue;
            }
            registry.slots[index] = this;
            file_invalidation_registered = true;
            ++registry.generation;
            if (registry.generation == 0)
            {
                registry.generation = 1;
            }
            registry.lock.release();
            return;
        }
        registry.lock.release();
        // 每个已发布 mm 至少对应一个 PCB，容量与进程池一致。
        // 溢出时不能静默跳过，否则 truncate 可留下旧 PTE。
        panic("ProcessMemoryManager: file invalidation registry exhausted");
    }

    void ProcessMemoryManager::retire_file_invalidation_registry()
    {
        FileInvalidationMmRegistry &registry = g_file_invalidation_mm_registry;
        registry.lock.acquire();
        if (file_invalidation_registered)
        {
            bool found = false;
            for (uint32 index = 0; index < k_file_invalidation_registry_capacity; ++index)
            {
                if (registry.slots[index] == this)
                {
                    registry.slots[index] = nullptr;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                registry.lock.release();
                panic("ProcessMemoryManager: registered mm missing from file registry");
            }
            file_invalidation_registered = false;
            ++registry.generation;
            if (registry.generation == 0)
            {
                registry.generation = 1;
            }
        }
        registry.lock.release();

        // exec 可能持有旧 mm 的递归 memory_lock 进入最终清理；
        // 而已 pin 的 truncate 扫描者正在等这把锁。摘 registry
        // 后先临时完整放锁，等 pin 归零再恢复原深度，避免环。
        const uint held_memory_lock_depth = memory_lock.is_holding()
                                                ? memory_lock_depth
                                                : 0;
        for (uint depth = 0; depth < held_memory_lock_depth; ++depth)
        {
            unlock_memory();
        }

        // 等待位于 registry spinlock 和 mm SleepLock 之外。有当前任务时
        // 主动让出 CPU，使单核上持 pin 的 truncate 扫描者能完成。
        if (file_invalidation_pins.load(eastl::memory_order_acquire) != 0 &&
            k_pm.get_cur_pcb() != nullptr &&
            Cpu::get_cpu()->get_num_off() != 0)
        {
            // 这只可能说明新调用点在持 spinlock 时销毁了已发布 mm。
            // 不能在关中断区域忙等/调度，明确 fail-stop 而不释放被 pin 的对象。
            panic("ProcessMemoryManager: retiring pinned mm while interrupts are disabled");
        }
        while (file_invalidation_pins.load(eastl::memory_order_acquire) != 0)
        {
            if (k_pm.get_cur_pcb() != nullptr &&
                Cpu::get_cpu()->get_num_off() == 0)
            {
                k_scheduler.yield();
            }
        }
        for (uint depth = 0; depth < held_memory_lock_depth; ++depth)
        {
            lock_memory();
        }
    }

    void ProcessMemoryManager::pin_file_invalidation()
    {
        const uint32 previous =
            file_invalidation_pins.fetch_add(1, eastl::memory_order_acq_rel);
        if (previous == ~static_cast<uint32>(0))
        {
            panic("ProcessMemoryManager: file invalidation pin overflow");
        }
    }

    void ProcessMemoryManager::unpin_file_invalidation()
    {
        const uint32 previous =
            file_invalidation_pins.fetch_sub(1, eastl::memory_order_acq_rel);
        if (previous == 0)
        {
            panic("ProcessMemoryManager: unmatched file invalidation unpin");
        }
    }

    void ProcessMemoryManager::begin_inactive_final_teardown()
    {
        /*
         * 这是跳过逐批 TLB shootdown 的唯一入口。ref_count==0 保证不存在
         * 合法的新执行者；active mask==0 保证此前运行该 mm 的 CPU 已离开。
         * user_asid 此时仍为 active，析构时只会转为 retired；tlb_mm.cc 在
         * ASID 真正复用前强制执行一次全核屏障，因此残留翻译不能指向复用页。
         */
        tlb_flush_lock.acquire();
        tlb_state_lock.acquire();
        const int references = ref_count.load(eastl::memory_order_acquire);
        const uint64 active_cpus = tlb_active_cpu_mask;
        const uint32 asid = user_asid;
        if (references != 0 || active_cpus != 0 || asid == 0)
        {
            tlb_state_lock.release();
            tlb_flush_lock.release();
            panic("ProcessMemoryManager: unsafe final teardown mm=%p refs=%d active=%p asid=%u",
                  this, references, active_cpus, asid);
        }
        inactive_final_teardown = true;
        tlb_state_lock.release();
        tlb_flush_lock.release();
    }

    void ProcessMemoryManager::reassert_inactive_final_teardown()
    {
        if (!inactive_final_teardown)
        {
            panic("ProcessMemoryManager: final teardown reasserted before begin mm=%p", this);
        }

        /*
         * 退出清理允许在文件/VMA 后端中 sleep。scheduler 会对 final-teardown
         * 任务跳过 enter_mm()；这里仍在每个无失效批次前执行 leave+复核，既
         * 覆盖 begin 之前残留的本 CPU 登记，也防止未来调度路径回归后静默复用页。
         */
        Pcb *current = k_pm.get_cur_pcb();
        if (current != nullptr && current->get_memory_manager() == this)
        {
            hal::tlb::leave_mm(*this);
        }

        tlb_flush_lock.acquire();
        tlb_state_lock.acquire();
        const int references = ref_count.load(eastl::memory_order_acquire);
        const uint64 active_cpus = tlb_active_cpu_mask;
        const uint32 asid = user_asid;
        if (references != 0 || active_cpus != 0 || asid == 0)
        {
            tlb_state_lock.release();
            tlb_flush_lock.release();
            panic("ProcessMemoryManager: final teardown invariant lost mm=%p refs=%d active=%p asid=%u",
                  this, references, active_cpus, asid);
        }
        tlb_state_lock.release();
        tlb_flush_lock.release();
    }

    void ProcessMemoryManager::rebuild_vma_index()
    {
        vma_index.clear();
        for (int i = 0; i < NVMA; ++i)
        {
            vma &entry = vma_data._vm[i];
            if (!entry.used)
            {
                continue;
            }

            if (!entry.valid_range())
            {
                printfRed("ProcessMemoryManager: rebuild_vma_index skip invalid slot=%d addr=%p len=%d used=%d\n",
                          i, (void *)entry.addr, entry.len, entry.used);
                continue;
            }

            if (!vma_index.insert(&entry))
            {
                printfRed("ProcessMemoryManager: rebuild_vma_index conflict slot=%d range=[%p,%p)\n",
                          i, (void *)entry.addr, (void *)entry.end_addr());
            }
        }
        if (!vm_space.rebuild_index())
        {
            printfRed("ProcessMemoryManager: rebuild_vma_index failed to rebuild VMASpace index\n");
        }
    }

    vma *ProcessMemoryManager::find_vma_covering(uint64 addr)
    {
        vma *space_vm = vm_space.find_vma_covering(addr);
        if (space_vm != nullptr || vma_index.empty())
        {
            return space_vm;
        }
        return vma_index.find(addr);
    }

    const vma *ProcessMemoryManager::find_vma_covering(uint64 addr) const
    {
        const vma *space_vm = vm_space.find_vma_covering(addr);
        if (space_vm != nullptr || vma_index.empty())
        {
            return space_vm;
        }
        return vma_index.find(addr);
    }

    vma *ProcessMemoryManager::find_first_vma_at_or_after(uint64 addr)
    {
        if (prefer_vm_space_lookup(*this, addr))
        {
            return vm_space.find_first_vma_at_or_after(addr);
        }
        if (vma_index.empty())
        {
            return vm_space.find_first_vma_at_or_after(addr);
        }
        return pick_lower_addr_vma(vma_index.lower_bound(addr), vm_space.find_first_vma_at_or_after(addr));
    }

    const vma *ProcessMemoryManager::find_first_vma_at_or_after(uint64 addr) const
    {
        if (prefer_vm_space_lookup(*this, addr))
        {
            return vm_space.find_first_vma_at_or_after(addr);
        }
        if (vma_index.empty())
        {
            return vm_space.find_first_vma_at_or_after(addr);
        }
        return pick_lower_addr_vma(vma_index.lower_bound(addr), vm_space.find_first_vma_at_or_after(addr));
    }

    vma *ProcessMemoryManager::find_prev_vma(uint64 start_addr)
    {
        if (prefer_vm_space_lookup(*this, start_addr))
        {
            return vm_space.find_prev_vma(start_addr);
        }
        if (vma_index.empty())
        {
            return vm_space.find_prev_vma(start_addr);
        }
        return pick_higher_addr_vma(vma_index.prev_by_start(start_addr), vm_space.find_prev_vma(start_addr));
    }

    const vma *ProcessMemoryManager::find_prev_vma(uint64 start_addr) const
    {
        if (prefer_vm_space_lookup(*this, start_addr))
        {
            return vm_space.find_prev_vma(start_addr);
        }
        if (vma_index.empty())
        {
            return vm_space.find_prev_vma(start_addr);
        }
        return pick_higher_addr_vma(vma_index.prev_by_start(start_addr), vm_space.find_prev_vma(start_addr));
    }

    vma *ProcessMemoryManager::find_next_vma(const vma *entry)
    {
        if (entry == nullptr)
        {
            return nullptr;
        }
        return find_first_vma_at_or_after(entry->end_addr());
    }

    const vma *ProcessMemoryManager::find_next_vma(const vma *entry) const
    {
        if (entry == nullptr)
        {
            return nullptr;
        }
        return find_first_vma_at_or_after(entry->end_addr());
    }

    bool ProcessMemoryManager::insert_vma_slot(vma &entry)
    {
        if (!entry.used || !entry.valid_range())
        {
            return false;
        }
        return vma_slot_index(&entry) >= 0 ? vma_index.insert(&entry) : vm_space.insert_area(entry);
    }

    void ProcessMemoryManager::erase_vma_slot(vma &entry, uint64 old_addr)
    {
        if (vma_slot_index(&entry) >= 0)
        {
            vma_index.erase(&entry, old_addr);
            return;
        }
        vm_space.erase_area(entry, old_addr);
    }

    bool ProcessMemoryManager::reindex_vma_slot(vma &entry, uint64 old_addr)
    {
        if (!entry.used)
        {
            return false;
        }
        if (vma_slot_index(&entry) >= 0)
        {
            vma_index.erase(&entry, old_addr);
            if (!entry.valid_range())
            {
                return false;
            }
            if (vma_index.insert(&entry))
            {
                return true;
            }

            // 发生冲突时回退到全量重建，确保索引不会停留在半更新状态。
            rebuild_vma_index();
            return false;
        }
        return entry.valid_range() && vm_space.reindex_area(entry, old_addr);
    }

    int ProcessMemoryManager::vma_slot_index(const vma *entry) const
    {
        if (entry == nullptr)
        {
            return -1;
        }
        const vma *base = &vma_data._vm[0];
        if (entry < base || entry >= base + NVMA)
        {
            return -1;
        }
        return static_cast<int>(entry - base);
    }

    bool ProcessMemoryManager::has_vma_conflict(uint64 start_addr, uint64 end_addr, const vma *ignore) const
    {
        if (vma_index.empty())
        {
            return vm_space.has_conflict(start_addr, end_addr, ignore);
        }
        return vma_index.has_conflict(start_addr, end_addr, ignore) ||
               vm_space.has_conflict(start_addr, end_addr, ignore);
    }

    int ProcessMemoryManager::fault_page(uint64 va, int access_type)
    {
        // trap/copyin/MAP_POPULATE 都可进入此函数。不依赖每个上层
        // 记得加锁；递归 memory_lock 使已加锁路径只增加 depth。
        class FaultMemoryLockGuard
        {
        public:
            explicit FaultMemoryLockGuard(ProcessMemoryManager &mm) : mm_(mm)
            {
                mm_.lock_memory();
            }
            ~FaultMemoryLockGuard()
            {
                mm_.unlock_memory();
            }

        private:
            ProcessMemoryManager &mm_;
        } fault_memory_guard(*this);

        uint64 page_va = PGROUNDDOWN(va);
        vma *vm = find_vma_covering(va);
        if (vm != nullptr && access_type == 1)
        {
            if ((vm->prot & PROT_WRITE) == 0)
            {
                return -1;
            }
            if (mem::k_vmm.resolve_cow_page(pagetable, page_va, this) == 0)
            {
                return 0;
            }
        }

        if (vm == nullptr)
        {
            Pcb *cur = k_pm.get_cur_pcb();
            uint64 user_sp = 0;
            if (cur != nullptr &&
                cur->get_memory_manager() == this &&
                cur->get_trapframe() != nullptr)
            {
                user_sp = cur->get_trapframe()->sp;
            }

            vma *candidate = find_first_vma_at_or_after(page_va + 1);
            if (candidate != nullptr)
            {
                if ((candidate->flags & MAP_GROWSDOWN) == 0 ||
                    page_va >= candidate->addr ||
                    !(user_sp >= page_va && user_sp < candidate->addr + PGSIZE))
                {
                    candidate = nullptr;
                }
            }

            if (candidate == nullptr)
            {
                printfRed("ProcessMemoryManager: no VMA found for fault va=%p access=%d\n",
                          (void *)va, access_type);
                return -1;
            }

            uint64 grow_len = candidate->addr - page_va;
            uint64 new_len = static_cast<uint64>(candidate->len) + grow_len;
            if (!candidate->is_expandable || new_len > candidate->max_len || new_len > 0x7fffffffULL)
            {
                return -1;
            }

            vma *prev = find_prev_vma(candidate->addr);
            constexpr uint64 growdown_guard_gap = 256 * PGSIZE;
            if (prev != nullptr)
            {
                uint64 prev_end = prev->end_addr();
                if ((prev_end <= candidate->addr && prev_end + growdown_guard_gap > prev_end &&
                     page_va < prev_end + growdown_guard_gap) ||
                    page_va < prev_end)
                {
                    return -1;
                }
            }

            uint64 old_addr = candidate->addr;
            const uint64 grow_pages = grow_len / PGSIZE;
            VmPrivateOverlayMap *shifted_overlay = nullptr;
            VmPrivateOverlayMap *old_overlay = candidate->private_page_overlay;
            if (old_overlay != nullptr && grow_pages != 0)
            {
                shifted_overlay = new VmPrivateOverlayMap();
                if (shifted_overlay == nullptr)
                {
                    return -1;
                }
                for (const auto &entry : *old_overlay)
                {
                    uint64 shifted_index = entry.first + grow_pages;
                    if (shifted_index < entry.first)
                    {
                        delete shifted_overlay;
                        return -1;
                    }
                    (*shifted_overlay)[shifted_index] = entry.second;
                }
            }

            candidate->addr = page_va;
            candidate->len = static_cast<int>(new_len);
            if (!reindex_vma_slot(*candidate, old_addr))
            {
                candidate->addr = old_addr;
                candidate->len = static_cast<int>(new_len - grow_len);
                delete shifted_overlay;
                return -1;
            }
            if (shifted_overlay != nullptr)
            {
                // VMA 起点向低地址移动后，相对页号也随之整体后移。
                // 已经驻留的私有页必须保持绑定原虚拟页，否则新长出的栈页会复用旧页槽。
                candidate->private_page_overlay = shifted_overlay;
                delete old_overlay;
            }
            vm = candidate;
        }

        return mem::k_vmm.allocate_vma_page(pagetable, va, vm, access_type);
    }

    void ProcessMemoryManager::zap_file_mappings_after_shrink(
        const fs::FilePageCacheIdentity &identity,
        uint64 new_size)
    {
        const uint64 first_file_page = PGROUNDDOWN(new_size) / PGSIZE;
        const uint64 zap_file_offset = first_file_page * PGSIZE;

        lock_memory();
        for_each_vma([&](vma &entry) -> bool
        {
            if (!entry.used || entry.object == nullptr ||
                entry.object->kind() != VmObjectKind::File)
            {
                return true;
            }

            auto *file_object = static_cast<FileVmObject *>(entry.object);
            if (!file_object->matches_cache_identity(identity))
            {
                return true;
            }

            // source key 是绝对文件页号。先判断 VMA 是否与截断
            // 后的范围相交，但每个匹配对象都可幂等退役其高页。
            const uint64 area_pages =
                (static_cast<uint64>(entry.len) + PGSIZE - 1) / PGSIZE;
            uint64 first_area_page = 0;
            if (zap_file_offset > entry.page_offset)
            {
                first_area_page = (zap_file_offset - entry.page_offset) / PGSIZE;
            }

            if (first_area_page < area_pages)
            {
                const uint64 first_va = PGROUNDDOWN(entry.addr) +
                                        first_area_page * PGSIZE;
                const uint64 end_va = PGROUNDUP(entry.end_addr());
                if (entry.has_resident_pages && pagetable.get_base() != 0 &&
                    first_va < end_va && end_va <= USER_MEMORY_TOP)
                {
                    // do_free=1 只归还 PTE 的 mapping ref；source/cache owner
                    // 与 overlay owner 在各自路径单独归还。
                    mem::k_vmm.vmunmap(pagetable,
                                       first_va,
                                       (end_va - first_va) / PGSIZE,
                                       1,
                                       mem::UnmapTlbMode::Invalidate);
                }

                if (entry.private_page_overlay != nullptr)
                {
                    for (auto it = entry.private_page_overlay->begin();
                         it != entry.private_page_overlay->end();)
                    {
                        auto current = it++;
                        if (current->first < first_area_page)
                        {
                            continue;
                        }
                        if (current->second != 0)
                        {
                            mem::k_pmm.free_page(user_page_kernel_ptr(current->second));
                        }
                        entry.private_page_overlay->erase(current);
                    }
                    if (entry.private_page_overlay->empty())
                    {
                        delete entry.private_page_overlay;
                        entry.private_page_overlay = nullptr;
                    }
                }
            }

            // 必须仍持有 mm lock，使 VMA 对 object 的引用在进入
            // object spinlock 前不会消失。锁序固定为 mm SleepLock -> object SpinLock。
            file_object->retire_source_pages_from(first_file_page);
            return true;
        });
        unlock_memory();
    }

    void invalidate_resident_file_mappings_after_shrink(
        const fs::FilePageCacheIdentity &identity,
        uint64 new_size)
    {
        if (identity.mount_identity == 0 || identity.inode == 0)
        {
            return;
        }

        FileInvalidationMmRegistry &registry = g_file_invalidation_mm_registry;
        for (;;)
        {
            registry.lock.acquire();
            const uint64 pass_generation = registry.generation;
            registry.lock.release();

            // 不在 16 KiB 内核栈放整个 mm 快照。逐 slot 在 registry
            // lock 下 pin，随即释放自旋锁再进入可睡眠 mm/TLB 路径。
            for (uint32 index = 0; index < k_file_invalidation_registry_capacity; ++index)
            {
                registry.lock.acquire();
                ProcessMemoryManager *mm = registry.slots[index];
                if (mm != nullptr)
                {
                    mm->pin_file_invalidation();
                }
                registry.lock.release();

                if (mm == nullptr)
                {
                    continue;
                }
                mm->zap_file_mappings_after_shrink(identity, new_size);
                mm->unpin_file_invalidation();
            }

            registry.lock.acquire();
            const bool stable = registry.generation == pass_generation;
            registry.lock.release();
            if (stable)
            {
                return;
            }
            // fork 可在本轮扫描期间发布从旧 PTE 复制的新 mm。
            // generation 变化时重扫；最终稳定检查后新建 mm 只能
            // 从已 zap 的父 mm 复制，或按新 EOF 重新 fault。
        }
    }

    uint64 ProcessMemoryManager::find_gap_in_vma_index(uint64 start_hint,
                                                       uint64 min_addr,
                                                       uint64 max_addr,
                                                       uint64 size,
                                                       uint64 alignment) const
    {
        if (prefer_vm_space_lookup(*this, start_hint))
        {
            return vm_space.find_gap(start_hint, min_addr, max_addr, size, alignment);
        }

        if (vma_index.empty())
        {
            return vm_space.find_gap(start_hint, min_addr, max_addr, size, alignment);
        }

        if (size == 0 || max_addr <= min_addr)
        {
            return 0;
        }

        uint64 candidate = start_hint < min_addr ? min_addr : start_hint;
        candidate = align_up_with_granularity(candidate, alignment);
        while (candidate < max_addr)
        {
            if (candidate + size < candidate || candidate + size > max_addr)
            {
                return 0;
            }

            const vma *covering = find_vma_covering(candidate);
            if (covering != nullptr)
            {
                candidate = align_up_with_granularity(covering->end_addr(), alignment);
                continue;
            }

            const vma *next = find_first_vma_at_or_after(candidate);
            if (next == nullptr || candidate + size <= next->addr)
            {
                return candidate;
            }

            candidate = align_up_with_granularity(next->end_addr(), alignment);
        }
        return 0;
    }

    bool ProcessMemoryManager::clone_vm_space_metadata_from(const ProcessMemoryManager &src)
    {
        return src.get_vm_space().for_each([&](const vma &entry) -> bool
        {
            return vm_space.clone_area_from(entry) != nullptr;
        });
    }

    bool ProcessMemoryManager::clone_private_vm_space_for_fork(ProcessMemoryManager &dst,
                                                                bool defer_parent_tlb_flush,
                                                                bool &parent_cow_changed,
                                                                eastl::vector<CowRollbackRange> &rollback_ranges)
    {
        bool copy_ok = true;
        if (!get_vm_space().for_each([&](const vma &entry) -> bool
        {
            if (!entry.valid_range())
            {
                return true;
            }

            // has_resident_pages 的 false 是“整段没有叶子映射”的强保证。
            // Rust/LLVM 会保留很大的惰性文件映射与匿名地址区；fork 无需为它们
            // 逐页 walk，也无需在 LoongArch 子页表中提前建立空层级。
            if (!entry.has_resident_pages)
            {
                return true;
            }

#ifdef LOONGARCH
            if (!dst.ensure_user_pagetable_hierarchy(entry.addr, static_cast<uint64>(entry.len)))
            {
                copy_ok = false;
                return false;
            }
#endif

            if (entry.object != nullptr && entry.object->shared_mapping())
            {
                if (!remap_shared_object_vma(dst, pagetable, entry))
                {
                    copy_ok = false;
                    return false;
                }
                return true;
            }

            int copy_result = mem::k_vmm.vm_copy(pagetable,
                                                 dst.pagetable,
                                                 entry.addr,
                                                 static_cast<uint64>(entry.len),
                                                 defer_parent_tlb_flush,
                                                 &rollback_ranges);
            if (copy_result < 0)
            {
                printfRed("[clone_private_vm_space_for_fork] copy area failed kind=%d addr=%p len=%d name=%s\n",
                          static_cast<int>(entry.area_kind),
                          (void *)entry.addr,
                          entry.len,
                          entry.debug_name != nullptr ? entry.debug_name : "(null)");
                copy_ok = false;
                return false;
            }
            parent_cow_changed |= copy_result > 0;

            if (entry.wipe_on_fork)
            {
                if (!wipe_child_vma_pages(dst, entry.addr, static_cast<uint64>(entry.len)))
                {
                    copy_ok = false;
                    return false;
                }
            }
            return true;
        }))
        {
            return false;
        }
        return copy_ok;
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
        // 把“父 PTE/VMA 快照 -> 子 mm registry 发布”放在同一个
        // 父 mm 锁临界区。外层 fork 已持锁时只增加递归深度。
        class CloneMemoryLockGuard
        {
        public:
            explicit CloneMemoryLockGuard(ProcessMemoryManager &mm) : mm_(mm)
            {
                mm_.lock_memory();
            }
            ~CloneMemoryLockGuard()
            {
                mm_.unlock_memory();
            }

        private:
            ProcessMemoryManager &mm_;
        } clone_memory_guard(*this);

        // 进程复制：创建新的内存管理器并深拷贝内容
        ProcessMemoryManager *new_mgr = new ProcessMemoryManager();

        // 为新进程创建页表
        if (!new_mgr->create_pagetable())
        {
            delete new_mgr;
            return nullptr;
        }
        // 唯一持有者不可能在另一颗 CPU 上同时执行用户态写入，因此可把多个
        // VMA 的同步 shootdown 合并为一次。多线程共享 mm 时仍保持逐段即时失效。
        const bool defer_parent_tlb_flush = get_ref_count() == 1;
        bool parent_cow_changed = false;
        eastl::vector<CowRollbackRange> cow_rollback_ranges;
        auto flush_deferred_parent_tlb = [&]()
        {
            if (defer_parent_tlb_flush && parent_cow_changed)
            {
                hal::tlb::flush_mm_range(*this, 0, 0);
                parent_cow_changed = false;
            }
        };
        auto restore_failed_fork_permissions = [&]() -> bool
        {
            bool restored = false;
            for (const CowRollbackRange &range : cow_rollback_ranges)
            {
                for (uint64 va = range.start; va < range.end; va += PGSIZE)
                {
                    mem::Pte pte = pagetable.walk(va, false);
                    if (pte.is_null() || !pte.is_valid())
                    {
                        continue;
                    }
#ifdef RISCV
                    const uint64 data = pte.get_data();
                    if ((data & mem::k_riscv_pte_cow) != 0)
                    {
                        pte.set_data((data | riscv::PteEnum::pte_writable_m) &
                                     ~mem::k_riscv_pte_cow);
                        restored = true;
                    }
#elif defined(LOONGARCH)
                    const uint64 data = pte.get_data();
                    if ((data & PTE_COW) != 0)
                    {
                        pte.set_data((data | PTE_W | PTE_D) & ~PTE_COW);
                        restored = true;
                    }
#endif
                }
            }
            return restored;
        };
        auto cleanup_failed_clone = [&]()
        {
            // 先释放所有子 PTE/overlay 引用，再精确撤销本次 fork 改过的父权限。
            new_mgr->emergency_cleanup();
            const bool restored_parent_permissions = restore_failed_fork_permissions();
            // 失败路径既可能撤销 COW，也可能保留原本已有共享者的 COW；一次
            // 全核失效同时覆盖两者，保证父线程恢复执行前权限与页表一致。
            if (parent_cow_changed || restored_parent_permissions)
            {
                hal::tlb::flush_mm_range(*this, 0, 0);
            }
            parent_cow_changed = false;
            delete new_mgr;
        };
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
        new_mgr->heap_high_watermark = heap_high_watermark;
        new_mgr->mmap_cursor = mmap_cursor;

        // 复制总内存大小
        new_mgr->total_memory_size = total_memory_size;

        // fork操作不共享虚拟内存，设置为false
        new_mgr->shared_vm = false;

        if (!new_mgr->clone_vm_space_metadata_from(*this))
        {
            printfRed("[clone_for_fork] clone VMASpace metadata failed\n");
            cleanup_failed_clone();
            return nullptr;
        }

        // 先按新的 VMASpace 私有区域复制已驻留页，并统一降级到页级 COW。
        // 这样 execve 新迁入的 PT_LOAD/堆/用户栈不再依赖 prog_sections/heap 特判。
        bool copy_success = true;
        if (!clone_private_vm_space_for_fork(*new_mgr,
                                             defer_parent_tlb_flush,
                                             parent_cow_changed,
                                             cow_rollback_ranges))
        {
            copy_success = false;
        }

        if (!copy_success)
        {
            printfRed("[clone_for_fork] copy VMASpace pages failed\n");
            cleanup_failed_clone();
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

            if (!vma_meta::clone_snapshot(new_mgr->vma_data._vm[i], vma_data._vm[i]))
            {
                cleanup_failed_clone();
                return nullptr;
            }
            new_mgr->vma_data._vm[i].owner_mm = new_mgr;

            if (!vma_data._vm[i].has_resident_pages)
            {
                continue;
            }

#ifdef LOONGARCH
            if (!new_mgr->ensure_user_pagetable_hierarchy(new_mgr->vma_data._vm[i].addr,
                                                          new_mgr->vma_data._vm[i].len))
            {
                cleanup_failed_clone();
                return nullptr;
            }
#endif

            // fork 必须保留父进程已经驻留的私有 VMA 页。
            // 动态链接器会在 MAP_PRIVATE 的 libc/ld.so GOT 页上写入重定位结果；
            // 如果这里只复制 VMA 元数据，子进程缺页时会重新从文件读原始 GOT，
            // 导致 _rtld_global 等指针退回 0 并在 glibc __fork 子分支崩溃。
            // 未驻留页仍保持惰性加载；MAP_SHARED/SHM 页由共享后端负责重新映射。
            if (vma_data._vm[i].object != nullptr && vma_data._vm[i].object->shared_mapping())
            {
                if (!remap_shared_object_vma(*new_mgr, pagetable, vma_data._vm[i]))
                {
                    cleanup_failed_clone();
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

                int copy_result = mem::k_vmm.vm_copy(pagetable,
                                                     new_mgr->pagetable,
                                                     vma_start,
                                                     vma_end - vma_start,
                                                     defer_parent_tlb_flush,
                                                     &cow_rollback_ranges);
                if (copy_result < 0)
                {
                    printfRed("[clone_for_fork] copy VMA %d failed addr=%p len=%d\n",
                              i, (void *)vma_data._vm[i].addr, vma_data._vm[i].len);
                    cleanup_failed_clone();
                    return nullptr;
                }
                parent_cow_changed |= copy_result > 0;

                if (vma_data._vm[i].wipe_on_fork)
                {
                    if (!wipe_child_vma_pages(*new_mgr,
                                              vma_data._vm[i].addr,
                                              static_cast<uint64>(vma_data._vm[i].len)))
                    {
                        cleanup_failed_clone();
                        return nullptr;
                    }
                }
            }
        }

        flush_deferred_parent_tlb();
        new_mgr->rebuild_vma_index();

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
        vma_index.clear();
        vm_space.clear();
        vm_space.init(this);

        // 重置堆信息
        heap_start = 0;
        heap_end = 0;
        heap_high_watermark = 0;
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
        heap_high_watermark = start_addr;
        reset_mmap_cursor(start_addr + k_mmap_guard_gap);

        if (vma *heap_area = vm_space.find_heap_area(); heap_area != nullptr)
        {
            vm_space.destroy_area(heap_area);
        }

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
        return has_vma_conflict(start_addr, end_addr, nullptr);
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

        uint64 upper_bound = USER_MEMORY_TOP;
        if (upper_bound > k_mmap_upper_guard)
        {
            upper_bound -= k_mmap_upper_guard;
        }
        upper_bound = PGROUNDDOWN(upper_bound);

        uint64 candidate = find_gap_in_vma_index(mmap_cursor,
                                                 minimum_start,
                                                 upper_bound,
                                                 aligned_size,
                                                 alignment);
        while (candidate != 0 && candidate < upper_bound)
        {
            uint64 candidate_end = candidate + aligned_size;
            if (candidate_end < candidate || candidate_end > upper_bound)
            {
                break;
            }

            bool overlaps_program_section = false;
            for (int i = 0; i < prog_section_count; ++i)
            {
                uint64 sec_start = PGROUNDDOWN((uint64)prog_sections[i]._sec_start);
                uint64 sec_end = PGROUNDUP((uint64)prog_sections[i]._sec_start + prog_sections[i]._sec_size);
                if (candidate < sec_end && candidate_end > sec_start)
                {
                    overlaps_program_section = true;
                    candidate = find_gap_in_vma_index(align_up_with_granularity(sec_end + PGSIZE, alignment),
                                                      minimum_start,
                                                      upper_bound,
                                                      aligned_size,
                                                      alignment);
                    break;
                }
            }

            if (!overlaps_program_section)
            {
                mmap_cursor = candidate_end;
                return candidate;
            }
        }

        printfRed("ProcessMemoryManager: no available mmap region for size=%p\n", (void *)aligned_size);
        return 0;
    }

    bool ProcessMemoryManager::ensure_user_pagetable_hierarchy(uint64 start, uint64 size)
    {
#ifdef LOONGARCH
        if (size == 0)
        {
            return true;
        }

        uint64 range_start = PGROUNDDOWN(start);
        uint64 range_end = PGROUNDUP(start + size);
        if (range_end < range_start || range_start >= USER_MEMORY_TOP || range_end > USER_MEMORY_TOP)
        {
            printfRed("ProcessMemoryManager: ensure_user_pagetable_hierarchy invalid range [%p, %p)\n",
                      (void *)range_start, (void *)range_end);
            return false;
        }

        // LoongArch 的 tlbr refill 入口要求中间层级先存在。
        // 这里不建立叶子映射，只把后续懒缺页需要的页表骨架补齐。
        for (uint64 va = range_start; va < range_end; va += PGSIZE)
        {
            mem::Pte pte_slot = pagetable.walk(va, true);
            if (pte_slot.is_null())
            {
                printfRed("ProcessMemoryManager: ensure_user_pagetable_hierarchy failed for va=%p\n",
                          (void *)va);
                return false;
            }
        }
#else
        (void)start;
        (void)size;
#endif
        return true;
    }

    void ProcessMemoryManager::unmap_heap_pages_in_range(const vma &entry, uint64 start, uint64 end)
    {
        if (start >= end || !pagetable.get_base())
        {
            return;
        }

        uint64 va_start = start <= entry.addr ? PGROUNDDOWN(entry.addr) : PGROUNDUP(start);
        uint64 va_end = PGROUNDUP(end);
        if (va_start >= va_end)
        {
            return;
        }

        if (inactive_final_teardown)
        {
            reassert_inactive_final_teardown();
        }
        const mem::UnmapTlbMode tlb_mode =
            inactive_final_teardown
                ? mem::UnmapTlbMode::SkipInactiveFinalTeardown
                : mem::UnmapTlbMode::Invalidate;
        uint64 run_start = 0;
        uint64 run_end = 0;
        auto flush_run = [&]()
        {
            if (run_start == run_end)
            {
                return;
            }
            mem::k_vmm.vmunmap(pagetable,
                               run_start,
                               (run_end - run_start) / PGSIZE,
                               1,
                               tlb_mode);
            run_start = run_end = 0;
        };

        for (uint64 va = va_start; va < va_end && va < USER_MEMORY_TOP; va += PGSIZE)
        {
            uint64 probe = va < entry.addr ? entry.addr : va;
            if (probe >= entry.end_addr())
            {
                flush_run();
                continue;
            }

            const vma *owner = find_vma_covering(probe);
            if (owner != &entry)
            {
                flush_run();
                continue;
            }

            if (run_start == run_end)
            {
                run_start = va;
            }
            run_end = va + PGSIZE;
        }
        flush_run();
    }

    bool ProcessMemoryManager::heap_growth_conflicts_with_mapping(uint64 start, uint64 end) const
    {
        if (start >= end)
        {
            return false;
        }

        uint64 cursor = start;
        while (cursor < end)
        {
            const vma *entry = find_vma_covering(cursor);
            if (entry == nullptr)
            {
                entry = find_first_vma_at_or_after(cursor);
            }

            if (entry == nullptr || entry->addr >= end)
            {
                return false;
            }

            if (entry->end_addr() <= cursor)
            {
                cursor += PGSIZE;
                continue;
            }

            /*
             * Linux 的 brk 只能扩展到下一段既有映射之前。文件私有映射同样
             * 是硬边界，不能因为它不是 MAP_SHARED 就让逻辑 program break
             * 穿过去；否则 malloc 会把共享库的只读段当成堆写坏。
             */
            if (entry->overlaps(start, end) &&
                entry->area_kind != VmAreaKind::Heap &&
                entry->end_addr() > heap_high_watermark)
            {
                return true;
            }

            cursor = entry->end_addr() < end ? entry->end_addr() : end;
        }

        return false;
    }

    bool ProcessMemoryManager::ensure_heap_metadata_for_range(uint64 start, uint64 end)
    {
        if (start >= end)
        {
            return true;
        }

        uint64 cursor = start;
        while (cursor < end)
        {
            vma *covering = find_vma_covering(cursor);
            if (covering != nullptr)
            {
                if (covering->area_kind == VmAreaKind::Heap)
                {
                    covering->prot = PROT_READ | PROT_WRITE;
                    covering->flags = MAP_PRIVATE | MAP_ANONYMOUS;
                    covering->grow_policy = VmGrowPolicy::Up;
                    covering->is_expandable = true;
                    covering->debug_name = "brk-heap";
                }
                cursor = covering->end_addr() < end ? covering->end_addr() : end;
                continue;
            }

            vma *next = find_first_vma_at_or_after(cursor);
            uint64 run_end = (next != nullptr && next->addr < end) ? next->addr : end;
            if (run_end <= cursor)
            {
                cursor += PGSIZE;
                continue;
            }

            vma *prev = find_prev_vma(cursor);
            if (prev != nullptr &&
                prev->area_kind == VmAreaKind::Heap &&
                prev->end_addr() == cursor)
            {
                uint64 new_len = static_cast<uint64>(prev->len) + (run_end - cursor);
                if (new_len > 0x7fffffffULL)
                {
                    return false;
                }
                prev->len = static_cast<int>(new_len);
                prev->prot = PROT_READ | PROT_WRITE;
                prev->flags = MAP_PRIVATE | MAP_ANONYMOUS;
                prev->grow_policy = VmGrowPolicy::Up;
                prev->is_expandable = true;
                prev->max_len = prev->max_len < new_len ? new_len : prev->max_len;
                prev->debug_name = "brk-heap";
                prev->has_resident_pages = true;
            }
            else
            {
                uint64 run_len = run_end - cursor;
                if (run_len > 0x7fffffffULL)
                {
                    return false;
                }

                vma *heap_area = vm_space.create_area(cursor,
                                                       run_len,
                                                       PROT_READ | PROT_WRITE,
                                                       MAP_PRIVATE | MAP_ANONYMOUS,
                                                       nullptr,
                                                       0,
                                                       VmAreaKind::Heap,
                                                       VmGrowPolicy::Up,
                                                       0,
                                                       "brk-heap");
                if (heap_area == nullptr)
                {
                    return false;
                }
                heap_area->is_expandable = true;
                heap_area->max_len = run_len;
                heap_area->has_resident_pages = true;
            }

            cursor = run_end;
        }

        return true;
    }

    void ProcessMemoryManager::trim_heap_metadata_to_end(uint64 new_end, bool unmap_pages)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            vma *entry = find_first_vma_at_or_after(0);
            while (entry != nullptr)
            {
                vma *next = find_next_vma(entry);
                if (entry->area_kind != VmAreaKind::Heap)
                {
                    entry = next;
                    continue;
                }

                uint64 entry_end = entry->end_addr();
                if (entry_end <= new_end)
                {
                    entry = next;
                    continue;
                }

                if (unmap_pages)
                {
                    uint64 remove_start = entry->addr >= new_end ? entry->addr : new_end;
                    unmap_heap_pages_in_range(*entry, remove_start, entry_end);
                }

                if (entry->addr >= new_end)
                {
                    uint64 old_addr = entry->addr;
                    if (vma_slot_index(entry) >= 0)
                    {
                        vma_meta::release_metadata(*entry);
                        reset_vma_entry(*entry);
                        erase_vma_slot(*entry, old_addr);
                    }
                    else
                    {
                        vm_space.destroy_area(entry);
                    }
                }
                else
                {
                    entry->len = static_cast<int>(new_end - entry->addr);
                    entry->has_resident_pages = true;
                }

                changed = true;
                break;
            }
        }
    }

    uint64 ProcessMemoryManager::grow_heap(uint64 new_end)
    {
        // 直接使用ProcessMemoryManager中的堆地址
        uint64 current_end = heap_end;
        if (new_end <= current_end)
        {
            return current_end; // 无需扩展
        }

        uint64 heap_limit = USER_MEMORY_TOP;
        if (heap_limit > k_mmap_guard_gap)
        {
            heap_limit -= k_mmap_guard_gap;
        }
        if (new_end >= heap_limit)
        {
            printfRed("ProcessMemoryManager: heap grow exceeds user space limit, new_end=%p\n", (void *)new_end);
            return current_end;
        }

        auto map_heap_page = [&](uint64 page_va) -> bool
        {
            if (is_page_mapped(page_va))
            {
                return true;
            }

            void *mem = mem::PhysicalMemoryManager::try_alloc_page();
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

        if (heap_growth_conflicts_with_mapping(current_end, new_end))
        {
            printfRed("ProcessMemoryManager: heap grow would cross an existing mapping, range=[%p, %p)\n",
                      (void *)current_end,
                      (void *)new_end);
            return current_end;
        }

        if (!ensure_heap_metadata_for_range(current_end, new_end))
        {
            printfRed("ProcessMemoryManager: failed to extend heap VMA metadata to %p\n",
                      (void *)new_end);
            trim_heap_metadata_to_end(current_end, false);
            return current_end;
        }

        uint64 first_page = PGROUNDDOWN(current_end);
        if (first_page < current_end)
        {
            first_page += PGSIZE;
        }
        auto rollback_heap_pages = [&](uint64 rollback_end)
        {
            for (uint64 rollback_va = first_page; rollback_va < rollback_end; rollback_va += PGSIZE)
            {
                const vma *covering_vm = find_vma_covering(rollback_va);
                if (covering_vm != nullptr && covering_vm->area_kind != VmAreaKind::Heap)
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
            const vma *covering_vm = find_vma_covering(va);
            if (covering_vm != nullptr)
            {
                if (covering_vm->area_kind != VmAreaKind::Heap)
                {
                    if (covering_vm->end_addr() > heap_high_watermark)
                    {
                        printfRed("ProcessMemoryManager: heap grow would cross VMA [%p, %p), kind=%d\n",
                                  (void *)covering_vm->addr,
                                  (void *)(covering_vm->addr + (uint64)covering_vm->len),
                                  static_cast<int>(covering_vm->area_kind));
                        rollback_heap_pages(va);
                        trim_heap_metadata_to_end(current_end, false);
                        return current_end;
                    }
                    // MAP_FIXED 在历史 brk 区间中留下的映射保持原所有者；
                    // program break 可以越过它，堆元数据只覆盖两侧空洞。
                    continue;
                }
            }

            if (!map_heap_page(va))
            {
                printfRed("ProcessMemoryManager: heap grow failed at page %p\n", (void *)va);
                // 内核栈只有 8KB，不能在这里维护一个“已分配页数组”。失败时重新遍历
                // 本次扩展过的区间，释放非 VMA 覆盖的 heap 页即可。
                rollback_heap_pages(va);
                trim_heap_metadata_to_end(current_end, false);
                return current_end;
            }
        }

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;
        if (heap_high_watermark < new_end)
        {
            heap_high_watermark = new_end;
        }
        reset_mmap_cursor(heap_end + k_mmap_guard_gap);

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

        // brk 收缩只回收 Heap 自己拥有的片段。MAP_FIXED/mmap 在 brk 区间里
        // 打出来的洞是独立 VMA，不能因为 program break 降低就被顺手拆掉。
        trim_heap_metadata_to_end(new_end, true);

        // 更新ProcessMemoryManager中的堆结束地址
        heap_end = new_end;

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

        free_vma_entry(&vma_data._vm[vma_index], true);
    }

    void ProcessMemoryManager::free_vma_entry(vma *entry, bool check_validity)
    {
        if (entry == nullptr || !entry->used)
        {
            return;
        }

        const int legacy_index = vma_slot_index(entry);
        vma &vm_entry = *entry;
        uint64 old_addr = vm_entry.addr;

        if (!is_reasonable_user_vma(vm_entry))
        {
            printfRed("ProcessMemoryManager: VMA 元数据异常，跳过页表释放并直接丢弃该条目 addr=%p len=%d legacy=%d\n",
                      (void *)vm_entry.addr,
                      vm_entry.len,
                      legacy_index >= 0 ? 1 : 0);
            if (legacy_index >= 0)
            {
                vma_meta::release_metadata(vm_entry);
                reset_vma_entry(vm_entry);
                erase_vma_slot(vm_entry, old_addr);
            }
            else
            {
                vm_space.destroy_area(entry);
            }
            return;
        }

        if (vm_entry.vfile != nullptr && !is_probably_kernel_object_ptr(vm_entry.vfile))
        {
            printfRed("ProcessMemoryManager: VMA 挂着异常文件指针 %p，跳过文件写回与 free_file\n",
                      vm_entry.vfile);
            vm_entry.vfile = nullptr;
        }

        // printfBlue("  Processing VMA %d: addr=%p, len=%u, vfd=%d, flags=0x%x, prot=0x%x\n",
        //            vma_index, (void *)vm_entry.addr, vm_entry.len,
        //            vm_entry.vfd, vm_entry.flags, vm_entry.prot);

        // 退出路径只负责撤销映射与回收引用；显式 msync/munmap 已覆盖文件写回。
        // 这里继续碰可能已经悬空的 vfile，收益远小于把内核直接打死的风险。

        if (is_shared_backed_vma(vm_entry))
        {
            if (release_shared_backed_vma(*this,
                                          &vm_entry,
                                          vm_entry,
                                          check_validity,
                                          check_validity ? "free_single_vma" : "emergency_cleanup") != 0)
            {
                return;
            }
        }
        else
        {
            uint64 va_start = PGROUNDDOWN(vm_entry.addr);
            uint64 va_end = PGROUNDUP(vm_entry.addr + vm_entry.len);
            if (vm_entry.has_resident_pages || vm_entry.area_kind == VmAreaKind::Heap)
            {
                unmap_vma_pages(vm_entry, va_start, va_end, check_validity);
            }
        }

        if (legacy_index >= 0)
        {
            vma_meta::release_metadata(vm_entry);
            reset_vma_entry(vm_entry);
            erase_vma_slot(vm_entry, old_addr);
            return;
        }

        vm_space.destroy_area(entry);
    }

    void ProcessMemoryManager::free_all_vma()
    {
        while (true)
        {
            vma *entry = find_first_vma_at_or_after(0);
            if (entry == nullptr)
            {
                break;
            }
            // VMA 重构后 ElfLoad/InterpreterLoad/Heap 的物理页也可能由
            // VmObject 与 private overlay 持有，必须先按 VMA 所有权释放。
            // program_sections 只作为旧式页表映射兜底，不能再跳过这些元数据。
            free_vma_entry(entry, true);
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

        eastl::vector<vma *> overlapping_vmas;
        vma *cursor = find_vma_covering(start_addr);
        if (cursor == nullptr)
        {
            cursor = find_first_vma_at_or_after(start_addr);
        }
        while (cursor != nullptr && cursor->addr < end_addr)
        {
            if (cursor->overlaps(start_addr, end_addr))
            {
                overlapping_vmas.push_back(cursor);
            }
            cursor = find_next_vma(cursor);
        }

        if (overlapping_vmas.empty())
        {
            printfYellow("ProcessMemoryManager: no VMA found for unmapping range\n");
            // 仍然尝试取消页表映射，以防有非VMA管理的映射
            safe_vmunmap(start_addr, end_addr, true);
            return 0;
        }

        // 处理每个重叠的VMA
        for (vma *vm_ptr : overlapping_vmas)
        {
            if (vm_ptr == nullptr || !vm_ptr->used)
            {
                continue;
            }
            vma &vm_entry = *vm_ptr;

            uint64 vma_start = vm_entry.addr;
            uint64 vma_end = vm_entry.addr + vm_entry.len;

            // 计算需要取消映射的区域
            uint64 unmap_start = start_addr > vma_start ? start_addr : vma_start;
            uint64 unmap_end = end_addr < vma_end ? end_addr : vma_end;

            // 如果需要写回文件映射
            if (vm_entry.vfile != nullptr &&
                (vm_entry.flags & MAP_SHARED) &&
                (vm_entry.prot & PROT_WRITE) != 0)
            {
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
                    int detach_result = release_shared_backed_vma(*this, &vm_entry, vm_entry, true, "munmap");
                    if (detach_result != 0)
                    {
                        return -1;
                    }
                }
                else
                {
                    if (vm_entry.has_resident_pages || vm_entry.area_kind == VmAreaKind::Heap)
                    {
                        unmap_vma_pages(vm_entry, unmap_start, unmap_end, true);
                    }
                }
            }
            else
            {
                if (vm_entry.has_resident_pages || vm_entry.area_kind == VmAreaKind::Heap)
                {
                    unmap_vma_pages(vm_entry, unmap_start, unmap_end, true);
                }
            }
            // 处理VMA条目的更新
            if (full_unmap)
            {
                // 完全取消映射
                uint64 old_vma_addr = vm_entry.addr;
                if (vm_entry.vfile != nullptr && !is_probably_live_file_object(vm_entry.vfile))
                {
                    printfRed("ProcessMemoryManager: VMA 的 vfile 指针异常，直接丢弃: %p\n",
                              vm_entry.vfile);
                    vm_entry.vfile = nullptr;
                }
                if (vma_slot_index(&vm_entry) >= 0)
                {
                    vma_meta::release_metadata(vm_entry);
                    reset_vma_entry(vm_entry);
                    erase_vma_slot(vm_entry, old_vma_addr);
                }
                else
                {
                    vm_space.destroy_area(&vm_entry);
                }
            }
            else
            {
                // 部分取消映射
                if (!partial_unmap_area(&vm_entry, unmap_start, unmap_end))
                {
                    printfRed("ProcessMemoryManager: partial unmap failed for VMA [%p, %p)\n",
                              (void *)vm_entry.addr,
                              (void *)vm_entry.end_addr());
                    return -1;
                }
            }
        }

        return 0;
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
        for_each_vma_in_range(start_addr, end_addr, [&](const vma &entry) -> bool
        {
            int slot = vma_slot_index(&entry);
            if (slot >= 0 && count < max_count)
            {
                overlapping_vmas[count++] = slot;
            }
            return count < max_count;
        });

        return count;
    }

    bool ProcessMemoryManager::partial_unmap_vma(int vma_index, uint64 unmap_start, uint64 unmap_end)
    {
        if (!is_vma_valid(vma_index))
        {
            return false;
        }
        return partial_unmap_area(&vma_data._vm[vma_index], unmap_start, unmap_end);
    }

    bool ProcessMemoryManager::partial_unmap_area(vma *entry, uint64 unmap_start, uint64 unmap_end)
    {
        if (entry == nullptr || !entry->used)
        {
            return false;
        }

        vma &vm_entry = *entry;
        const bool legacy_area = vma_slot_index(entry) >= 0;
        uint64 vma_start = vm_entry.addr;
        uint64 vma_end = vm_entry.addr + vm_entry.len;
        uint64 total_pages = PGROUNDUP(static_cast<uint64>(vm_entry.len)) / PGSIZE;

        if (unmap_start == vma_start && unmap_end < vma_end)
        {
            vma source_view = vm_entry;
            uint64 old_addr = vm_entry.addr;
            uint64 removed_bytes = unmap_end - vma_start;
            uint64 removed_pages = removed_bytes / PGSIZE;
            VmPrivateOverlayMap *remaining_overlay =
                vma_meta::clone_overlay_subset(source_view, removed_pages, total_pages - removed_pages, false);

            vma_meta::release_overlay_pages_in_range(source_view, 0, removed_pages);
            vma_meta::discard_overlay_container(vm_entry);
            vm_entry.private_page_overlay = remaining_overlay;
            vm_entry.addr = unmap_end;
            vm_entry.len = vma_end - unmap_end;
            if (vm_entry.object != nullptr)
            {
                vm_entry.page_offset += removed_bytes;
            }
            if (vm_entry.vfile != nullptr)
            {
                vm_entry.offset += removed_bytes;
            }
            vm_entry.file_backed_bytes =
                segment_file_backed_bytes(source_view, removed_bytes, static_cast<uint64>(vm_entry.len));
            return reindex_vma_slot(vm_entry, old_addr);
        }
        if (unmap_start > vma_start && unmap_end == vma_end)
        {
            vma source_view = vm_entry;
            uint64 keep_pages = (unmap_start - vma_start) / PGSIZE;
            VmPrivateOverlayMap *remaining_overlay =
                vma_meta::clone_overlay_subset(source_view, 0, keep_pages, false);

            vma_meta::release_overlay_pages_in_range(source_view, keep_pages, total_pages - keep_pages);
            vma_meta::discard_overlay_container(vm_entry);
            vm_entry.private_page_overlay = remaining_overlay;
            vm_entry.len = unmap_start - vma_start;
            vm_entry.file_backed_bytes =
                segment_file_backed_bytes(source_view, 0, static_cast<uint64>(vm_entry.len));
            return true;
        }
        if (unmap_start > vma_start && unmap_end < vma_end)
        {
            const uint64 front_pages = (unmap_start - vma_start) / PGSIZE;
            const uint64 back_start_page = (unmap_end - vma_start) / PGSIZE;
            const uint64 back_pages = total_pages - back_start_page;
            const uint64 removed_pages = back_start_page - front_pages;

            vma source_view = vm_entry;
            VmPrivateOverlayMap *front_overlay =
                vma_meta::clone_overlay_subset(source_view, 0, front_pages, false);
            VmPrivateOverlayMap *back_overlay =
                vma_meta::clone_overlay_subset(source_view, back_start_page, back_pages, false);

            vma *new_vm = nullptr;
            vm_entry.len = unmap_start - vma_start;
            vm_entry.file_backed_bytes =
                segment_file_backed_bytes(source_view, 0, static_cast<uint64>(vm_entry.len));

            if (legacy_area)
            {
                int new_vma_idx = -1;
                for (int j = 0; j < NVMA; ++j)
                {
                    if (!vma_data._vm[j].used)
                    {
                        new_vma_idx = j;
                        break;
                    }
                }
                if (new_vma_idx < 0)
                {
                    vm_entry.len = source_view.len;
                    vm_entry.file_backed_bytes = source_view.file_backed_bytes;
                    return false;
                }

                new_vm = &vma_data._vm[new_vma_idx];
                *new_vm = source_view;
                new_vm->used = 1;
                new_vm->addr = unmap_end;
                new_vm->len = vma_end - unmap_end;
                new_vm->private_page_overlay = back_overlay;
                if (new_vm->object != nullptr)
                {
                    new_vm->object->get();
                    new_vm->page_offset = source_view.page_offset + (unmap_end - vma_start);
                }
                if (source_view.vfile != nullptr)
                {
                    source_view.vfile->dup();
                    new_vm->offset = source_view.offset + (unmap_end - vma_start);
                }
                else
                {
                    new_vm->offset = source_view.offset;
                }
                new_vm->file_backed_bytes =
                    segment_file_backed_bytes(source_view,
                                              unmap_end - vma_start,
                                              static_cast<uint64>(new_vm->len));
                if (!insert_vma_slot(*new_vm))
                {
                    vma_meta::release_metadata(*new_vm);
                    reset_vma_entry(*new_vm);
                    vm_entry.len = source_view.len;
                    vm_entry.file_backed_bytes = source_view.file_backed_bytes;
                    return false;
                }
            }
            else
            {
                VmObject *new_object = source_view.object;
                if (new_object != nullptr)
                {
                    new_object->get();
                }
                new_vm = vm_space.create_area(unmap_end,
                                              vma_end - unmap_end,
                                              source_view.prot,
                                              source_view.flags,
                                              new_object,
                                              source_view.page_offset + (unmap_end - vma_start),
                                              source_view.area_kind,
                                              source_view.grow_policy,
                                              source_view.guard_pages,
                                              source_view.debug_name);
                if (new_vm == nullptr)
                {
                    // create_area() 接管 new_object 引用；失败时它已经完成引用归还。
                    vm_entry.len = source_view.len;
                    vm_entry.file_backed_bytes = source_view.file_backed_bytes;
                    return false;
                }
                if (source_view.vfile != nullptr)
                {
                    source_view.vfile->dup();
                    new_vm->vfile = source_view.vfile;
                    new_vm->offset = source_view.offset + (unmap_end - vma_start);
                }
                else
                {
                    new_vm->vfile = nullptr;
                    new_vm->offset = source_view.offset;
                }
                new_vm->vfd = source_view.vfd;
                new_vm->max_len = source_view.max_len;
                new_vm->is_expandable = source_view.is_expandable;
                new_vm->backing_kind = source_view.backing_kind;
                new_vm->backing_shmid = source_view.backing_shmid;
                new_vm->backing_base = source_view.backing_base;
                new_vm->has_resident_pages = source_view.has_resident_pages;
                new_vm->wipe_on_fork = source_view.wipe_on_fork;
                new_vm->advice_state = source_view.advice_state;
                new_vm->zero_fill_past_file = source_view.zero_fill_past_file;
                new_vm->file_backed_bytes =
                    segment_file_backed_bytes(source_view,
                                              unmap_end - vma_start,
                                              static_cast<uint64>(new_vm->len));
                new_vm->private_page_overlay = back_overlay;
            }

            vma_meta::release_overlay_pages_in_range(source_view, front_pages, removed_pages);
            vma_meta::discard_overlay_container(vm_entry);
            vm_entry.private_page_overlay = front_overlay;
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
        special_mappings_ready = true;
        return true;
    }

    bool ProcessMemoryManager::ensure_special_mappings()
    {
        if (!pagetable.get_base())
        {
            return false;
        }
        if (special_mappings_ready)
        {
            return true;
        }

#ifdef RISCV
        mem::Pte trampoline_pte = pagetable.walk(TRAMPOLINE, false);
        if (trampoline_pte.is_null() || !trampoline_pte.is_valid())
        {
            if (!mem::k_vmm.map_pages(pagetable,
                                      TRAMPOLINE,
                                      PGSIZE,
                                      (uint64)trampoline,
                                      riscv::PteEnum::pte_readable_m | riscv::pte_executable_m))
            {
                printfRed("ProcessMemoryManager: repair trampoline mapping failed, pt=%p\n",
                          (void *)pagetable.get_base());
                return false;
            }
        }

        mem::Pte sig_trampoline_pte = pagetable.walk(SIG_TRAMPOLINE, false);
        if (sig_trampoline_pte.is_null() || !sig_trampoline_pte.is_valid())
        {
            if (!mem::k_vmm.map_pages(pagetable,
                                      SIG_TRAMPOLINE,
                                      PGSIZE,
                                      (uint64)sig_trampoline,
                                      riscv::PteEnum::pte_readable_m |
                                          riscv::pte_executable_m |
                                          riscv::PteEnum::pte_user_m))
            {
                printfRed("ProcessMemoryManager: repair sig trampoline mapping failed, pt=%p\n",
                          (void *)pagetable.get_base());
                return false;
            }
        }
#elif defined(LOONGARCH)
        mem::Pte sig_trampoline_pte = pagetable.walk(SIG_TRAMPOLINE, false);
        if (sig_trampoline_pte.is_null() || !sig_trampoline_pte.is_valid())
        {
            if (!mem::k_vmm.map_pages(pagetable,
                                      SIG_TRAMPOLINE,
                                      PGSIZE,
                                      (uint64)sig_trampoline,
                                      PTE_P | PTE_MAT | PTE_D | PTE_U))
            {
                printfRed("ProcessMemoryManager: repair sig trampoline mapping failed, pt=%p\n",
                          (void *)pagetable.get_base());
                return false;
            }
        }
#endif

        special_mappings_ready = true;
        return true;
    }

    void ProcessMemoryManager::free_pagetable()
    {
        special_mappings_ready = false;
        if (!pagetable.get_base())
        {
            printfYellow("ProcessMemoryManager: pagetable already released, skip free_pagetable\n");
            return;
        }

        mem::PageTable &pt = pagetable;
        if (inactive_final_teardown)
        {
            reassert_inactive_final_teardown();
        }
        const mem::UnmapTlbMode tlb_mode =
            inactive_final_teardown
                ? mem::UnmapTlbMode::SkipInactiveFinalTeardown
                : mem::UnmapTlbMode::Invalidate;

        // 阶段1：不再依赖分散的引用计数，直接释放
        // 取消特殊页面的映射
#ifdef RISCV
        mem::k_vmm.vmunmap(pt, TRAMPOLINE, 1, 0, tlb_mode);
#endif
        // 每个 PCB 槽位在共享地址空间中都有一页独立 trapframe 映射。
        // 页表即将释放，逐个拆掉可避免 freewalk 遗留叶子映射。
        for (uint gid = 0; gid < num_process; ++gid)
        {
            mem::k_vmm.vmunmap(pt, USER_TRAPFRAME(gid), 1, 0, tlb_mode);
        }
        mem::k_vmm.vmunmap(pt, SIG_TRAMPOLINE, 1, 0, tlb_mode);

        pt.freewalk();
        pagetable.set_base(0);

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

        // 用户态普通映射绝不能触碰每线程 trapframe / signal trampoline / trampoline 保留区。
        // 如果上层把长度、VMA 或堆边界算错了，这里至少要把错误限制在普通用户区，
        // 不能因为一次错误的 munmap 把整个进程返回用户态所依赖的固定映射拆掉。
        if (va_start >= USER_MEMORY_TOP)
        {
            printf("\33[1;31mProcessMemoryManager: safe_vmunmap 请求进入保留区，已拒绝 start=%p end=%p\33[0m\n",
                   (void *)va_start, (void *)va_end);
            return;
        }
        if (va_end > USER_MEMORY_TOP)
        {
            printf("\33[1;31mProcessMemoryManager: safe_vmunmap 请求跨越保留区，自动截断 start=%p end=%p -> %p\33[0m\n",
                   (void *)va_start, (void *)va_end, (void *)USER_MEMORY_TOP);
            va_end = USER_MEMORY_TOP;
        }
        if (va_start >= va_end)
        {
            return;
        }

        // vmunmap 本身会跳过不存在/无效的 PTE，因此 check_validity 不再需要
        // 让调用方先逐页 walk；保留参数只为维持现有调用契约。
        (void)check_validity;
        if (inactive_final_teardown)
        {
            reassert_inactive_final_teardown();
        }
        const mem::UnmapTlbMode tlb_mode =
            inactive_final_teardown
                ? mem::UnmapTlbMode::SkipInactiveFinalTeardown
                : mem::UnmapTlbMode::Invalidate;

        uint64 cursor = va_start;
        while (cursor < va_end)
        {
            const vma *covering = find_vma_covering(cursor);
            uint64 run_end = va_end;
            bool do_free = true;
            if (covering != nullptr)
            {
                do_free = mapping_pages_should_be_freed_on_unmap(*covering);
                const uint64 covering_end = PGROUNDUP(covering->end_addr());
                if (covering_end < run_end)
                {
                    run_end = covering_end;
                }
            }
            else
            {
                const vma *next = find_first_vma_at_or_after(cursor);
                if (next != nullptr && next->addr < va_end)
                {
                    const uint64 next_start = PGROUNDDOWN(next->addr);
                    if (next_start > cursor)
                    {
                        run_end = next_start;
                    }
                    else
                    {
                        // VMA 理论上均页对齐；保守处理同页起点，避免坏元数据
                        // 让循环停滞或错误释放共享后端页。
                        do_free = mapping_pages_should_be_freed_on_unmap(*next);
                        const uint64 next_end = PGROUNDUP(next->end_addr());
                        run_end = next_end < va_end ? next_end : va_end;
                    }
                }
            }

            if (run_end <= cursor)
            {
                panic("ProcessMemoryManager: safe_vmunmap made no progress cursor=%p end=%p",
                      cursor, va_end);
            }
            mem::k_vmm.vmunmap(pagetable,
                               cursor,
                               (run_end - cursor) / PGSIZE,
                               do_free ? 1 : 0,
                               tlb_mode);
            cursor = run_end;
        }
    }

    void ProcessMemoryManager::unmap_vma_pages(const vma &entry,
                                               uint64 va_start,
                                               uint64 va_end,
                                               bool check_validity)
    {
        if (!pagetable.get_base())
        {
            return;
        }

        va_start = PGROUNDDOWN(va_start);
        va_end = PGROUNDUP(va_end);
        if (va_start >= va_end)
        {
            return;
        }

        if (va_start >= USER_MEMORY_TOP)
        {
            return;
        }
        if (va_end > USER_MEMORY_TOP)
        {
            va_end = USER_MEMORY_TOP;
        }

        // vmunmap 会自行忽略未驻留页；普通连续 VMA 直接交给其内部 256 页
        // 批处理，避免上层每页一次 shootdown 和 PMM 锁竞争。
        (void)check_validity;
        if (inactive_final_teardown)
        {
            reassert_inactive_final_teardown();
        }
        const mem::UnmapTlbMode tlb_mode =
            inactive_final_teardown
                ? mem::UnmapTlbMode::SkipInactiveFinalTeardown
                : mem::UnmapTlbMode::Invalidate;

        // 匿名私有映射如果已经有 resident overlay，就只拆真正 fault 过的页；
        // 不再像传统做法那样把整个 VMA 范围从头扫到尾。
        if (entry.object == nullptr &&
            entry.private_page_overlay != nullptr &&
            entry.is_private_mapping())
        {
            uint64 base = PGROUNDDOWN(entry.addr);
            uint64 request_start = PGROUNDDOWN(va_start);
            uint64 request_end = PGROUNDUP(va_end);
            constexpr uint32 k_sparse_unmap_batch_pages = 256;
            uint64 sparse_addresses[k_sparse_unmap_batch_pages]{};
            uint32 sparse_count = 0;
            auto flush_sparse = [&]()
            {
                if (sparse_count == 0)
                {
                    return;
                }
                mem::k_vmm.vmunmap_sparse(pagetable,
                                          sparse_addresses,
                                          sparse_count,
                                          1,
                                          tlb_mode);
                sparse_count = 0;
            };

            for (const auto &overlay_entry : *entry.private_page_overlay)
            {
                if (overlay_entry.second == 0)
                {
                    continue;
                }

                uint64 va = base + overlay_entry.first * PGSIZE;
                if (va < base || va >= entry.end_addr() || va >= USER_MEMORY_TOP ||
                    va < request_start || va >= request_end)
                {
                    continue;
                }

                sparse_addresses[sparse_count++] = va;
                if (sparse_count == k_sparse_unmap_batch_pages)
                {
                    flush_sparse();
                }
            }
            flush_sparse();
            return;
        }

        const bool do_free = mapping_pages_should_be_freed_on_unmap(entry);
        mem::k_vmm.vmunmap(pagetable,
                           va_start,
                           (va_end - va_start) / PGSIZE,
                           do_free ? 1 : 0,
                           tlb_mode);
    }

    /****************************************************************************************
     * 统一内存释放接口实现
     ****************************************************************************************/

    bool ProcessMemoryManager::free_all_memory()
    {
        /*
         * fetch_sub 的旧值唯一决定最终清理者。不能让调用者在返回后再读
         * ref_count 决定 delete：refs=2 时，非最后线程可能先返回，随后看到
         * 另一个 CPU 已经把 refs 降到 0，进而在真正清理者仍使用 mm 时提前
         * 析构对象。
         */
        const int old_count = ref_count.fetch_sub(1, eastl::memory_order_acq_rel);
        if (old_count <= 0)
        {
            panic("ProcessMemoryManager::free_all_memory underflow mm=%p old_refs=%d asid=%u",
                  this, old_count, user_asid);
        }

        if (old_count == 1)
        {
            // 线程共享地址空间路径里，如果引用计数已经漂掉，但进程池里仍有其他 PCB
            // 指向当前 mm，就绝不能继续 free 页表，否则会把仍在运行的线程直接打死。
            // 所有正常 release 路径都必须先从 PCB 摘掉当前持有者，所以这里的
            // 每一个 holder 都是真正尚未归还的引用，而不包含当前最终清理者。
            int holders = count_live_mm_holders(this);
            if (holders > 0)
            {
                ref_count.store(holders, eastl::memory_order_release);
                shared_vm = true;
                printfYellow("ProcessMemoryManager: refcount drift repaired, mm=%p holders=%d remaining=%d\n",
                             this, holders, holders);
                return false;
            }

            // 只有最后一个业务引用真正进入销毁时才摘 registry。
            // 先禁止新 truncate snapshot，再等待已有 pin 离开，然后
            // 才能释放 VMA/object/页表。
            retire_file_invalidation_registry();

            // 最后一个地址空间引用已归还，且 cleanup_memory_manager() 已让
            // 当前 CPU 离开 mm。只有这个断言成立，后续批量撤 PTE 才可依赖
            // “ASID 退休后、全核屏障前不复用”而省略逐批 shootdown。
            begin_inactive_final_teardown();

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
            return true;
        }

        shared_vm = true;
        // 如果引用计数还大于0，说明还有其他进程/线程在使用这块内存，不进行释放
        return false;
    }

    void ProcessMemoryManager::emergency_cleanup()
    {
        printfRed("ProcessMemoryManager: emergency cleanup\n");

        // 只用于未发布/唯一持有的创建失败 mm。若 PCB 已经
        // 短暂发布过，也要在释放 VMA 前摘除并等待 snapshot pin。
        retire_file_invalidation_registry();

        // 紧急清理：不进行写回操作，只释放内存

        // 1. 强制释放VMA（不写回）
        while (true)
        {
            vma *entry = find_first_vma_at_or_after(0);
            if (entry == nullptr)
            {
                break;
            }
            // 紧急清理同样不能留下程序段/堆 VMA；否则页表会被 freewalk 丢掉，
            // VmObject 持有的页引用却不会和映射引用成对回收。
            free_vma_entry(entry, false);
        }
        vma_index.clear();
        vm_space.rebuild_index();
        shared_vm = false;

        // 2. 释放其他内存资源
        if (pagetable.get_base())
        {
            free_all_program_sections();
            free_heap_memory();
            free_pagetable();
        }

        reset_memory_sections();

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

        // 遍历所有已记录的程序段，释放其占用的内存
        for (int i = 0; i < section_count; i++)
        {
            if (section_descs[i]._sec_start && section_descs[i]._sec_size > 0)
            {
                uint64 va_start = PGROUNDDOWN((uint64)section_descs[i]._sec_start);
                uint64 va_end = PGROUNDUP((uint64)section_descs[i]._sec_start + section_descs[i]._sec_size);

                // vmunmap 会跳过惰性未驻留页；整个连续段只做内部批量失效与
                // PMM 归还，避免错误回滚路径逐页广播 TLB。
                if (va_end > va_start)
                {
                    mem::k_vmm.vmunmap(pagetable,
                                       va_start,
                                       (va_end - va_start) / PGSIZE,
                                       1);
                }
            }
        }

        // 清理页表的特殊映射（trampoline、sig_trampoline等）
#ifdef RISCV
        mem::k_vmm.vmunmap(pagetable, TRAMPOLINE, 1, 0);
#endif
        // 失败的 exec 加载也可能已经让共享页表中的线程返回过用户态；
        // 清理所有固定槽位，避免 freewalk 留下 trapframe 叶子映射。
        for (uint gid = 0; gid < num_process; ++gid)
        {
            mem::k_vmm.vmunmap(pagetable, USER_TRAPFRAME(gid), 1, 0);
        }
        mem::k_vmm.vmunmap(pagetable, SIG_TRAMPOLINE, 1, 0);

        // 阶段1：不再使用分散的引用计数
        // pagetable.dec_ref(); // 注释掉分散的引用计数操作

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
        if (fs::k_vfs.fstat(vma_entry.vfile, &st) == 0)
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

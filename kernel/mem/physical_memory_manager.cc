#include "physical_memory_manager.hh"
#include "types.hh"
#include "platform.hh"
#include "devs/spinlock.hh"
#include "buddysystem.hh"
#include "printer.hh"
#include "klib.hh"
#include "slab.hh"
#include "platform.hh"
#include "devs/dtb.hh"
extern "C" char end[]; // 来自链接脚本
#ifdef RISCV
extern uint64 k_dtb_addr;
#endif
extern uint64 k_initrd_start;
extern uint64 k_initrd_end;

namespace mem
{
    PhysicalMemoryManager k_pmm;
    uint64 PhysicalMemoryManager::pa_start;
    SpinLock PhysicalMemoryManager::memlock;
    BuddySystem *PhysicalMemoryManager::_buddy;
    uint32 PhysicalMemoryManager::page_count;
    uint32 PhysicalMemoryManager::heap_page_count;
    uint64 PhysicalMemoryManager::phys_top;
    uint64 PhysicalMemoryManager::kernel_linear_top;
    uint64 PhysicalMemoryManager::heap_area_start;
    uint64 PhysicalMemoryManager::heap_area_size;
    uint64 PhysicalMemoryManager::heap_allocator_size;
    uint64 PhysicalMemoryManager::shm_start;
    uint64 PhysicalMemoryManager::shm_size;

    namespace
    {
        uint16 *k_page_refcounts = nullptr;
        uint64 k_page_refcount_bytes = 0;

        constexpr uint64 align_up(uint64 value, uint64 alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        bool checked_region_top(uint64 base, uint64 size, uint64 &top)
        {
            if (size == 0 || base > ~0ULL - size)
            {
                return false;
            }
            top = base + size;
            return true;
        }

        struct ReservedRange
        {
            uint64 start = 0;
            uint64 end = 0;
        };

        struct UsableInterval
        {
            uint64 start = 0;
            uint64 top = 0;

            uint64 bytes() const
            {
                return top > start ? top - start : 0;
            }
        };

        UsableInterval largest_usable_interval(uint64 raw_start, uint64 raw_top,
                                               const ReservedRange *reserved,
                                               int reserved_count)
        {
            const uint64 start = PGROUNDUP(raw_start);
            const uint64 top = PGROUNDDOWN(raw_top);
            if (top <= start)
            {
                return {};
            }

            // 当前启动协议只需要排除 DTB 与 initrd。复制后排序，避免依赖
            // 固件放置二者的先后顺序；重叠区间也会在 cursor 推进时自然合并。
            ReservedRange sorted[2]{};
            int count = reserved_count > 2 ? 2 : reserved_count;
            for (int index = 0; index < count; ++index)
            {
                sorted[index] = reserved[index];
            }
            if (count == 2 && sorted[1].start < sorted[0].start)
            {
                ReservedRange temporary = sorted[0];
                sorted[0] = sorted[1];
                sorted[1] = temporary;
            }

            UsableInterval best{};
            uint64 cursor = start;
            auto consider = [&](uint64 gap_start, uint64 gap_top) {
                gap_start = PGROUNDUP(gap_start);
                gap_top = PGROUNDDOWN(gap_top);
                if (gap_top > gap_start && gap_top - gap_start > best.bytes())
                {
                    best.start = gap_start;
                    best.top = gap_top;
                }
            };

            for (int index = 0; index < count; ++index)
            {
                if (sorted[index].end <= cursor || sorted[index].start >= top)
                {
                    continue;
                }

                const uint64 reserved_start = PGROUNDDOWN(sorted[index].start);
                const uint64 reserved_end = PGROUNDUP(sorted[index].end);
                if (reserved_start > cursor)
                {
                    consider(cursor, reserved_start < top ? reserved_start : top);
                }
                if (reserved_end > cursor)
                {
                    cursor = reserved_end;
                }
                if (cursor >= top)
                {
                    break;
                }
            }
            consider(cursor, top);
            return best;
        }

        struct PmmMetadataLayout
        {
            uint64 metadata_start = 0;
            uint64 metadata_end = 0;
            uint64 tree_start = 0;
            uint64 tree_bytes = 0;
            uint64 refcount_start = 0;
            uint64 refcount_bytes = 0;
            uint64 managed_start = 0;
            uint32 managed_pages = 0;
        };

        PmmMetadataLayout calculate_pmm_layout(uint64 metadata_start, uint64 managed_limit)
        {
            if (managed_limit <= metadata_start + PGSIZE * 16)
            {
                panic("[pmm] region too small for dynamic metadata");
            }

            uint64 candidate_pages = (managed_limit - metadata_start) / PGSIZE;
            if (candidate_pages > UINT32_MAX)
            {
                panic("[pmm] region has too many pages for 32-bit buddy offsets: %lu",
                      candidate_pages);
            }

            PmmMetadataLayout layout{};
            layout.metadata_start = metadata_start;
            uint32 managed_pages = static_cast<uint32>(candidate_pages);

            // refcount 数量依赖最终 managed pages，而 metadata 页又会减少 managed
            // pages。迭代到固定点，通常两轮即可收敛。
            for (int iteration = 0; iteration < 16; ++iteration)
            {
                layout.tree_start = align_up(metadata_start + sizeof(BuddySystem), alignof(uint64));
                layout.tree_bytes = BuddySystem::required_tree_bytes(managed_pages);
                layout.refcount_start = align_up(layout.tree_start + layout.tree_bytes, alignof(uint16));
                layout.refcount_bytes = static_cast<uint64>(managed_pages) * sizeof(uint16);
                if (layout.refcount_start > ~0ULL - layout.refcount_bytes)
                {
                    panic("[pmm] metadata address overflow");
                }
                layout.metadata_end = PGROUNDUP(layout.refcount_start + layout.refcount_bytes);
                if (layout.metadata_end >= managed_limit)
                {
                    panic("[pmm] metadata consumes allocator region: start=%p end=%p limit=%p",
                          metadata_start, layout.metadata_end, managed_limit);
                }

                uint64 next_pages64 = (managed_limit - layout.metadata_end) / PGSIZE;
                if (next_pages64 > UINT32_MAX)
                {
                    panic("[pmm] managed page count overflow: %lu", next_pages64);
                }
                uint32 next_pages = static_cast<uint32>(next_pages64);
                if (next_pages == managed_pages)
                {
                    layout.managed_start = layout.metadata_end;
                    layout.managed_pages = managed_pages;
                    return layout;
                }
                managed_pages = next_pages;
            }

            panic("[pmm] dynamic metadata layout did not converge");
        }

        struct HeapLayout
        {
            uint64 metadata_bytes = 0;
            uint64 allocator_bytes = 0;
            uint64 shm_bytes = 0;
            uint32 allocator_pages = 0;
        };

        HeapLayout calculate_heap_layout(uint64 area_bytes)
        {
            HeapLayout layout{};
            uint64 desired_shm = area_bytes / 3;
            if (desired_shm > SHM_SIZE)
            {
                desired_shm = SHM_SIZE;
            }
            desired_shm = PGROUNDDOWN(desired_shm);

            const uint32 maximum_heap_pages = static_cast<uint32>(vm_kernel_heap_size / PGSIZE);
            uint64 heap_budget = area_bytes > desired_shm ? area_bytes - desired_shm : 0;
            uint32 heap_pages = static_cast<uint32>(heap_budget / PGSIZE);
            if (heap_pages > maximum_heap_pages)
            {
                heap_pages = maximum_heap_pages;
            }

            for (int iteration = 0; iteration < 16; ++iteration)
            {
                uint64 metadata_bytes = PGROUNDUP(BuddySystem::required_storage_bytes(heap_pages));
                uint64 next_pages64 = heap_budget > metadata_bytes
                                          ? (heap_budget - metadata_bytes) / PGSIZE
                                          : 0;
                if (next_pages64 > maximum_heap_pages)
                {
                    next_pages64 = maximum_heap_pages;
                }
                uint32 next_pages = static_cast<uint32>(next_pages64);
                if (next_pages == heap_pages)
                {
                    layout.metadata_bytes = metadata_bytes;
                    layout.allocator_pages = heap_pages;
                    layout.allocator_bytes = static_cast<uint64>(heap_pages) * PGSIZE;
                    uint64 remaining = area_bytes - metadata_bytes - layout.allocator_bytes;
                    layout.shm_bytes = PGROUNDDOWN(remaining > SHM_SIZE ? SHM_SIZE : remaining);
                    return layout;
                }
                heap_pages = next_pages;
            }

            panic("[pmm] heap metadata layout did not converge");
        }

        inline uint64 normalize_managed_page_addr(uint64 addr)
        {
#ifdef LOONGARCH
            // LoongArch 页表叶子里记录的是纯物理地址，但 PMM/buddy 管的是 DMWIN 直映地址。
            // 如果这里不统一折算，释放用户页时就会把错误页号塞回 buddy，长跑下会表现成
            // “仍在使用的用户页被重新分配给 trapframe/kstack”。
            if (addr != 0 && addr < PHYSBASE)
            {
                addr = to_vir(addr);
            }
#endif
            return addr;
        }

        inline bool page_index_in_range(uint64 page_index)
        {
            return page_index < PhysicalMemoryManager::get_page_count() &&
                   k_page_refcounts != nullptr;
        }
    } // namespace

    uint64 PhysicalMemoryManager::pa2pgnm(void *pa)
    {
        auto addr = normalize_managed_page_addr(reinterpret_cast<uint64>(pa));
        if (addr % PGSIZE != 0)
        {
            panic("pa2pgnm: address is not page-aligned");
        }
        return (addr - pa_start) / PGSIZE;
    }

    void *PhysicalMemoryManager::pgnm2pa(int pgnm)
    {
        return reinterpret_cast<void *>(static_cast<uint64>(pgnm) * PGSIZE + pa_start);
    }

    int PhysicalMemoryManager::size_to_page_num(uint64 size)
    {
        return static_cast<int>(size / PGSIZE + (size % PGSIZE != 0));
    }

    void PhysicalMemoryManager::init()
    {
        memlock.init("memlock");
        uint64 initrd_start = k_initrd_start;
        uint64 initrd_end = k_initrd_end;
        if (initrd_start == 0 || initrd_end <= initrd_start)
        {
            // RISC-V 启动路径不会预先扫描 initrd；在扩大到 DTB 全量内存后必须
            // 显式保留它，否则 PMM 会在文件系统挂载前覆盖 initrd 内容。
            DtbManager::get_initrd(initrd_start, initrd_end);
        }
        if (initrd_start != 0 && initrd_end > initrd_start)
        {
            // 后续 VMM 与文件系统都要使用同一组物理边界。get_initrd() 只通过
            // 引用返回结果，不会替调用方更新这两个全局值，因此在 PMM 选区前
            // 统一发布，避免“已从 allocator 排除、分页后却没有映射”的裂缝。
            k_initrd_start = initrd_start;
            k_initrd_end = initrd_end;
        }

        ReservedRange reserved_ranges[2]{};
        int reserved_count = 0;
        uint64 dtb_size = DtbManager::get_dtb_size();
        if (k_dtb_addr != 0)
        {
            // DTB header 异常时保守沿用旧实现的 2 MiB 保护窗；正常路径只
            // 排除 totalsize 覆盖的页面，不再把 DTB 后面的全部 RAM 丢掉。
            if (dtb_size == 0)
            {
                dtb_size = _1M * 2;
            }
            uint64 dtb_end = 0;
            if (!checked_region_top(k_dtb_addr, dtb_size, dtb_end))
            {
                panic("[pmm] invalid DTB reserved range: start=%p size=%p",
                      k_dtb_addr, dtb_size);
            }
            reserved_ranges[reserved_count++] = {k_dtb_addr, dtb_end};
        }
        if (initrd_start != 0 && initrd_end > initrd_start)
        {
            reserved_ranges[reserved_count++] = {initrd_start, initrd_end};
        }

        DtbMemoryRegion regions[DtbManager::k_max_memory_regions]{};
        int region_count = DtbManager::get_memory_regions(regions, DtbManager::k_max_memory_regions);
        uint64 allocator_start = 0;
        uint64 allocator_top = 0;

#ifdef RISCV
        const uint64 kernel_end_phys = PGROUNDUP(reinterpret_cast<uint64>(end));
        int kernel_region_index = -1;
        uint64 kernel_region_top = 0;
        uint64 selected_region_top = 0;
        UsableInterval best_interval{};
        for (int i = 0; i < region_count; ++i)
        {
            uint64 region_top = 0;
            if (!checked_region_top(regions[i].base, regions[i].size, region_top))
            {
                printfYellow("[pmm] ignoring invalid dtb memory region[%d]\n", i);
                continue;
            }
            const bool contains_kernel =
                kernel_end_phys > regions[i].base && kernel_end_phys <= region_top;
            if (contains_kernel)
            {
                kernel_region_index = i;
                kernel_region_top = PGROUNDDOWN(region_top);
            }

            const uint64 candidate_start = contains_kernel
                                               ? kernel_end_phys
                                               : regions[i].base;
            UsableInterval candidate = largest_usable_interval(
                candidate_start, region_top, reserved_ranges, reserved_count);
            if (candidate.bytes() > best_interval.bytes())
            {
                best_interval = candidate;
                selected_region_top = PGROUNDDOWN(region_top);
            }
        }

        if (kernel_region_index < 0 || best_interval.bytes() == 0)
        {
            // 没有可解析 DTB 时保留原有启动边界，避免把未知地址当作 RAM。
            best_interval = largest_usable_interval(
                kernel_end_phys, PHYSTOP, reserved_ranges, reserved_count);
            if (best_interval.bytes() == 0)
            {
                panic("[pmm] fallback RAM has no usable interval");
            }
            kernel_region_top = PGROUNDDOWN(PHYSTOP);
            selected_region_top = kernel_region_top;
            printfYellow("[pmm] incomplete DTB RAM description, fallback top=%p\n",
                         kernel_region_top);
        }
        allocator_start = best_interval.start;
        allocator_top = best_interval.top;
        // RISC-V 没有 DMWIN，页表必须同时覆盖内核所在 RAM 与最终选中的
        // allocator RAM。即使 DTB/initrd 位于二者之间，也只映射而不分配。
        kernel_linear_top = kernel_region_top > selected_region_top
                                ? kernel_region_top
                                : selected_region_top;

#elif defined(LOONGARCH)
        const uint64 kernel_end_phys = PGROUNDUP(VIRT2PHY(reinterpret_cast<uint64>(end)));
        int kernel_region_index = -1;
        for (int i = 0; i < region_count; ++i)
        {
            uint64 region_top = 0;
            if (!checked_region_top(regions[i].base, regions[i].size, region_top))
            {
                printfYellow("[pmm] ignoring invalid dtb memory region[%d]\n", i);
                continue;
            }
            if (kernel_end_phys > regions[i].base && kernel_end_phys <= region_top)
            {
                kernel_region_index = i;
                kernel_linear_top = to_vir(PGROUNDDOWN(region_top));
            }
        }

        UsableInterval best_interval{};
        for (int i = 0; i < region_count; ++i)
        {
            uint64 region_top = 0;
            if (!checked_region_top(regions[i].base, regions[i].size, region_top))
            {
                continue;
            }
            const uint64 region_start = i == kernel_region_index
                                            ? kernel_end_phys
                                            : regions[i].base;
            UsableInterval candidate = largest_usable_interval(
                region_start, region_top, reserved_ranges, reserved_count);
            if (candidate.bytes() > best_interval.bytes())
            {
                best_interval = candidate;
            }
        }

        if (best_interval.bytes() == 0)
        {
            best_interval = largest_usable_interval(
                kernel_end_phys, VIRT2PHY(PHYSTOP),
                reserved_ranges, reserved_count);
            if (best_interval.bytes() == 0)
            {
                panic("[pmm] LoongArch fallback RAM has no usable interval");
            }
            allocator_start = to_vir(best_interval.start);
            allocator_top = to_vir(best_interval.top);
            kernel_linear_top = PGROUNDDOWN(PHYSTOP);
            printfYellow("[pmm] no usable DTB memory region, fallback=%p-%p\n",
                         allocator_start, allocator_top);
        }
        else
        {
            // DMWIN 对所有 RAM 提供直映；因此页分配器应使用最大连续 RAM 区，
            // 而不是永远困在包含内核的 128/256 MiB 低端区。
            allocator_start = to_vir(best_interval.start);
            allocator_top = to_vir(best_interval.top);
        }
        if (kernel_linear_top == 0)
        {
            kernel_linear_top = PGROUNDDOWN(PHYSTOP);
        }
#endif

        allocator_start = PGROUNDUP(allocator_start);
        allocator_top = PGROUNDDOWN(allocator_top);
        if (allocator_top <= allocator_start + PGSIZE * 64)
        {
            panic("[pmm] insufficient allocator region: start=%p top=%p",
                  allocator_start, allocator_top);
        }
        phys_top = allocator_top;

        const uint32 max_heap_pages = static_cast<uint32>(vm_kernel_heap_size / PGSIZE);
        const uint64 max_heap_metadata = PGROUNDUP(BuddySystem::required_storage_bytes(max_heap_pages));
        const uint64 max_heap_region = max_heap_metadata + vm_kernel_heap_size + SHM_SIZE;
        const uint32 min_heap_pages = static_cast<uint32>((_1M * 8) / PGSIZE);
        const uint64 min_heap_metadata = PGROUNDUP(BuddySystem::required_storage_bytes(min_heap_pages));
        const uint64 min_heap_region = min_heap_metadata + _1M * 8 + _1M * 2;

        uint64 available_bytes = allocator_top - allocator_start;
        uint64 heap_region_bytes = available_bytes / 3;
        if (heap_region_bytes < min_heap_region)
            heap_region_bytes = min_heap_region;
        if (heap_region_bytes > max_heap_region)
            heap_region_bytes = max_heap_region;
        if (heap_region_bytes + PGSIZE * 64 > available_bytes)
            heap_region_bytes = available_bytes / 3;

        heap_area_start = PGROUNDDOWN(allocator_top - heap_region_bytes);
        heap_area_size = allocator_top - heap_area_start;
        PmmMetadataLayout pmm_layout = calculate_pmm_layout(allocator_start, heap_area_start);
        _buddy = reinterpret_cast<BuddySystem *>(pmm_layout.metadata_start);
        pa_start = pmm_layout.managed_start;
        page_count = pmm_layout.managed_pages;
        k_page_refcounts = reinterpret_cast<uint16 *>(pmm_layout.refcount_start);
        k_page_refcount_bytes = pmm_layout.refcount_bytes;
        memset(reinterpret_cast<void *>(pmm_layout.metadata_start), 0,
               pmm_layout.metadata_end - pmm_layout.metadata_start);

        HeapLayout heap_layout = calculate_heap_layout(heap_area_size);
        heap_allocator_size = heap_layout.allocator_bytes;
        heap_page_count = heap_layout.allocator_pages;
        shm_start = heap_area_start + heap_layout.metadata_bytes + heap_allocator_size;
        shm_size = heap_layout.shm_bytes;
        heap_area_size = heap_layout.metadata_bytes + heap_allocator_size + shm_size;

        if (heap_page_count == 0 || shm_size == 0)
        {
            panic("[pmm] heap/shm space too small (heap pages=%d, shm=%p)",
                  heap_page_count, shm_size);
        }

        _buddy->Initialize(pa_start, page_count,
                           reinterpret_cast<void *>(pmm_layout.tree_start),
                           pmm_layout.tree_bytes);
    }

    void *PhysicalMemoryManager::alloc_page()
    {
        void *pa = try_alloc_page();
        if (pa == nullptr)
        {
            panic("[pmm] alloc_page failed");
        }
        return pa;
    }

    uint64 PhysicalMemoryManager::get_free_page_count()
    {
        memlock.acquire();
        const uint64 free_pages = _buddy == nullptr ? 0 : _buddy->get_free_page_count();
        memlock.release();
        return free_pages;
    }

    void *PhysicalMemoryManager::try_alloc_page_impl(bool clear)
    {
        // Buddy 的树和页引用计数描述的是同一批页面。单核时期仅保护引用计数
        // 不会出问题，但 SMP 下两个 CPU 同时递归修改 Buddy 树会把页号算坏，
        // 最终可能把 0x1000 之类非 RAM 地址交给调用方。
        memlock.acquire();
        int x = _buddy->Alloc(0);
        if (x == -1)
        {
            memlock.release();
            return nullptr;
        }
        if (!page_index_in_range(static_cast<uint64>(x)))
        {
            memlock.release();
            panic("[pmm] buddy returned invalid page index=%d pages=%d", x, page_count);
        }

        void *pa = pgnm2pa(x);
        if (k_page_refcounts[x] != 0)
        {
            panic("[pmm] buddy allocated page with live refcount: page=%p index=%d ref=%u caller=%p",
                  pa,
                  x,
                  k_page_refcounts[x],
                  __builtin_return_address(0));
        }
        k_page_refcounts[x] = 1;
        memlock.release();
        if (clear)
        {
            memset(pa, 0, PGSIZE);
        }
        return pa;
    }

    void *PhysicalMemoryManager::try_alloc_page()
    {
        return try_alloc_page_impl(true);
    }

    void *PhysicalMemoryManager::try_alloc_page_uninitialized()
    {
        return try_alloc_page_impl(false);
    }

    void *PhysicalMemoryManager::alloc_pages(int count)
    {
        void *pa = try_alloc_pages(count);
        if (pa == nullptr)
        {
            panic("[pmm] alloc_pages failed, count=%d", count);
        }

        return pa;
    }

    void *PhysicalMemoryManager::try_alloc_pages(int count)
    {
        if (count <= 0)
        {
            return nullptr;
        }

        // 连续页接口由 free_pages() 成块释放；块内每页都记录 owner 引用，
        // 且 Buddy 元数据必须与单页路径使用同一把锁更新。
        memlock.acquire();
        int page_index = _buddy->Alloc(count);
        if (page_index >= 0)
        {
            BuddySystem::PageQueryResult block =
                _buddy->query_page(static_cast<uint32>(page_index));
            if (!block.in_range || block.is_free ||
                block.block_offset != static_cast<uint32>(page_index))
            {
                memlock.release();
                panic("[pmm] invalid contiguous allocation metadata index=%d", page_index);
            }
            for (uint32 offset = 0; offset < block.block_pages; ++offset)
            {
                const uint32 index = static_cast<uint32>(page_index) + offset;
                if (index >= page_count || k_page_refcounts[index] != 0)
                {
                    memlock.release();
                    panic("[pmm] contiguous allocation overlaps live page index=%u ref=%u",
                          index,
                          index < page_count ? k_page_refcounts[index] : 0);
                }
                // 连续内核缓冲的每一页都持有一份块 owner 引用。
                k_page_refcounts[index] = 1;
            }
        }
        memlock.release();
        if (page_index < 0)
        {
            return nullptr;
        }
        if (!page_index_in_range(static_cast<uint64>(page_index)))
        {
            panic("[pmm] buddy returned invalid contiguous page index=%d pages=%d", page_index, page_count);
        }

        void *pa = pgnm2pa(page_index);
        memset(pa, 0, static_cast<uint64>(count) * PGSIZE);
        return pa;
    }

    void PhysicalMemoryManager::free_page1(void *pa, uint64 size)
    {

        auto addr = reinterpret_cast<uint64>(pa);
        if (addr % PGSIZE != 0)
        {

            SlabAllocator::dealloc(pa, size);

            return;
        }
        const uint64 page_index = pa2pgnm(pa);
        memlock.acquire();
        BuddySystem::PageQueryResult block =
            _buddy->query_page(static_cast<uint32>(page_index));
        if (!block.in_range || block.is_free || block.block_offset != page_index)
        {
            memlock.release();
            panic("[pmm] free_page1 requires allocation start pa=%p index=%lu size=%lu",
                  pa, page_index, size);
        }
        for (uint32 offset = 0; offset < block.block_pages; ++offset)
        {
            const uint32 index = static_cast<uint32>(page_index) + offset;
            if (index >= page_count || k_page_refcounts[index] != 1)
            {
                memlock.release();
                panic("[pmm] free_page1 block still shared index=%u ref=%u size=%lu",
                      index,
                      index < page_count ? k_page_refcounts[index] : 0,
                      size);
            }
            k_page_refcounts[index] = 0;
        }
        _buddy->Free(static_cast<int>(page_index));
        memlock.release();
    }

    void PhysicalMemoryManager::free_page(void *pa)
    {
        // printfCyan("释放物理页:  %p\n", pa);
        uint64 page_index = pa2pgnm(pa);
        if (!page_index_in_range(page_index))
        {
            printfRed("[pmm] free_page out of managed range: pa=%p index=%d pages=%d\n",
                      pa, static_cast<int>(page_index), page_count);
            return;
        }

        bool should_free = true;
        memlock.acquire();
        BuddySystem::PageQueryResult block =
            _buddy->query_page(static_cast<uint32>(page_index));
        if (!block.in_range || block.is_free)
        {
            memlock.release();
            panic("[pmm] free_page on unallocated buddy page pa=%p index=%lu",
                  pa, page_index);
        }
        if (block.block_pages > 1)
        {
            if (block.block_offset != page_index)
            {
                memlock.release();
                panic("[pmm] free_page requires block start pa=%p index=%lu block=%u+%u",
                      pa, page_index, block.block_offset, block.block_pages);
            }
            for (uint32 offset = 0; offset < block.block_pages; ++offset)
            {
                const uint32 index = static_cast<uint32>(page_index) + offset;
                if (index >= page_count || k_page_refcounts[index] != 1)
                {
                    memlock.release();
                    panic("[pmm] kernel block still shared index=%u ref=%u",
                          index,
                          index < page_count ? k_page_refcounts[index] : 0);
                }
                k_page_refcounts[index] = 0;
            }
            _buddy->Free(static_cast<int>(page_index));
            memlock.release();
            return;
        }
        if (k_page_refcounts[page_index] > 1)
        {
            --k_page_refcounts[page_index];
            should_free = false;
        }
        else
        {
            k_page_refcounts[page_index] = 0;
        }
        if (should_free)
        {
            _buddy->Free(page_index);
        }
        memlock.release();
    }

    void PhysicalMemoryManager::free_pages(void *pa)
    {
        if (pa == nullptr)
        {
            return;
        }

        auto addr = reinterpret_cast<uint64>(pa);
        if (addr % PGSIZE != 0)
        {
            panic("[pmm] free_pages requires page-aligned address: %p", pa);
        }

        const uint64 page_index = pa2pgnm(pa);
        memlock.acquire();
        BuddySystem::PageQueryResult block =
            _buddy->query_page(static_cast<uint32>(page_index));
        if (!block.in_range || block.is_free || block.block_offset != page_index)
        {
            memlock.release();
            panic("[pmm] free_pages requires allocation start pa=%p index=%lu",
                  pa, page_index);
        }
        for (uint32 offset = 0; offset < block.block_pages; ++offset)
        {
            const uint32 index = static_cast<uint32>(page_index) + offset;
            if (index >= page_count || k_page_refcounts[index] != 1)
            {
                memlock.release();
                panic("[pmm] free_pages block still shared index=%u ref=%u",
                      index,
                      index < page_count ? k_page_refcounts[index] : 0);
            }
            k_page_refcounts[index] = 0;
        }
        _buddy->Free(static_cast<int>(page_index));
        memlock.release();
    }

    bool PhysicalMemoryManager::retain_page(void *pa)
    {
        uint64 page_index = pa2pgnm(pa);
        if (!page_index_in_range(page_index))
        {
            return false;
        }

        memlock.acquire();
        if (k_page_refcounts[page_index] == 0 || k_page_refcounts[page_index] == UINT16_MAX)
        {
            memlock.release();
            return false;
        }
        ++k_page_refcounts[page_index];
        memlock.release();
        return true;
    }

    uint64 PhysicalMemoryManager::retain_pages_batch(void *const *pages, uint32 count)
    {
        if (pages == nullptr || count == 0)
        {
            return 0;
        }
        if (count > 64)
        {
            panic("[pmm] retain_pages_batch count exceeds bitmap: %u", count);
        }

        uint64 retained_mask = 0;
        memlock.acquire();
        for (uint32 index = 0; index < count; ++index)
        {
            uint64 page_index = pa2pgnm(pages[index]);
            if (!page_index_in_range(page_index) ||
                k_page_refcounts[page_index] == 0 ||
                k_page_refcounts[page_index] == UINT16_MAX)
            {
                continue;
            }
            ++k_page_refcounts[page_index];
            retained_mask |= 1ULL << index;
        }
        memlock.release();
        return retained_mask;
    }

    uint16 PhysicalMemoryManager::page_ref_count(void *pa)
    {
        uint64 page_index = pa2pgnm(pa);
        if (!page_index_in_range(page_index))
        {
            return 0;
        }

        memlock.acquire();
        uint16 refcount = k_page_refcounts[page_index];
        memlock.release();
        return refcount;
    }

    bool PhysicalMemoryManager::is_managed_page(void *pa)
    {
        uint64 addr = normalize_managed_page_addr(reinterpret_cast<uint64>(pa));
        if (addr < pa_start || addr >= pa_start + static_cast<uint64>(page_count) * PGSIZE)
        {
            return false;
        }
        if ((addr % PGSIZE) != 0)
        {
            return false;
        }
        return ((addr - pa_start) / PGSIZE) < page_count && k_page_refcounts != nullptr;
    }
    void PhysicalMemoryManager::clear_page(void *pa)
    {
        uint64 *p = (uint64 *)pa;
        const uint cnt = PGSIZE >> 3;
        for (uint i = 0; i < cnt; i++)
            p[i] = 0;
    }

    void *PhysicalMemoryManager::kmalloc_impl(size_t size, bool clear)
    {
        int page_num = size_to_page_num(size);
        // printfCyan("kmalloc: size = %lu, page_num = %d\n", size, page_num);
        
        // 检查请求的页数是否合理
        if ((uint32)page_num > page_count) {
            printfRed("kmalloc: request too many pages (%d > %d)\n", page_num, page_count);
            return 0;
        }
        
        memlock.acquire();
        int x = _buddy->Alloc(page_num);
        if (x >= 0)
        {
            BuddySystem::PageQueryResult block =
                _buddy->query_page(static_cast<uint32>(x));
            if (!block.in_range || block.is_free ||
                block.block_offset != static_cast<uint32>(x))
            {
                memlock.release();
                panic("[pmm] invalid kmalloc block metadata index=%d", x);
            }
            for (uint32 offset = 0; offset < block.block_pages; ++offset)
            {
                const uint32 index = static_cast<uint32>(x) + offset;
                if (index >= page_count || k_page_refcounts[index] != 0)
                {
                    memlock.release();
                    panic("[pmm] kmalloc overlaps live page index=%u ref=%u",
                          index,
                          index < page_count ? k_page_refcounts[index] : 0);
                }
                k_page_refcounts[index] = 1;
            }
        }
        memlock.release();
        // printfCyan("kmalloc: buddy返回的页号 x = %d\n", x);
        
        if (x == -1)
        {
            printfRed("kmalloc: alloc failed, size = %lu\n", size);
            return 0; // 分配失败
        }
        else
        {
            // 检查返回的页号是否在合理范围内
            if (x < 0 || (uint32)x >= page_count) {
                printfRed("kmalloc: 警告！buddy返回的页号超出范围: %d (应该在0-%d之间)\n", x, page_count-1);
                return 0;
            }
            
            void *pa = pgnm2pa(x);
            if (clear)
            {
                // 默认 kmalloc() 维持零初始化契约；只有明确整段覆盖的 IO
                // 临时缓冲才允许跳过这次预清零。
                memset(pa, 0, (size_t)page_num * PGSIZE);
            }
            return pa;
        }
        // }
        // else if(size < PGSIZE)
        // {
        //     //there maybe some bugs to be fixed
        //     return SlabAllocator::alloc(size);
        // }
        // else
        // {
        //     panic("kmalloc: size is too large");
        //     return nullptr; // 永远不会执行到这里，但必须有返回值
        // }
    }

    void *PhysicalMemoryManager::kmalloc(size_t size)
    {
        return kmalloc_impl(size, true);
    }

    void *PhysicalMemoryManager::kmalloc_uninitialized(size_t size)
    {
        return kmalloc_impl(size, false);
    }

    void *PhysicalMemoryManager::kcalloc(uint n, size_t size)
    {
        void *pa = kmalloc(n * size);
        if (pa == nullptr)
        {
            return nullptr;
        }
        return pa;
    }

    PhysicalMemoryManager::PageDebugInfo PhysicalMemoryManager::debug_query_page(void *pa)
    {
        PageDebugInfo info{};
        if (pa == nullptr)
        {
            return info;
        }

        uint64 input_addr = reinterpret_cast<uint64>(pa);
        info.page_pa = PGROUNDDOWN(input_addr);
        info.aligned = (input_addr % PGSIZE) == 0;

        if (_buddy == nullptr)
        {
            return info;
        }

        uint64 managed_addr = info.page_pa;
#ifdef LOONGARCH
        // LoongArch 页表里拿到的是物理地址，而 buddy/PMM 管的是 DMWIN 直映虚拟地址。
        // 调试查询时统一折算成内核线性地址，再判断是否落在 buddy 管辖范围内。
        if (managed_addr < PHYSBASE)
        {
            managed_addr = to_vir(managed_addr);
        }
#endif

        const uint64 managed_top =
            pa_start + static_cast<uint64>(page_count) * PGSIZE;
        if (managed_addr < pa_start || managed_addr >= managed_top)
        {
            return info;
        }

        info.managed = true;
        info.page_offset = (managed_addr - pa_start) / PGSIZE;
        memlock.acquire();
        info.buddy = _buddy->query_page(static_cast<uint32>(info.page_offset));
        memlock.release();
        return info;
    }

}

#include "physical_memory_manager.hh"
#include "types.hh"
#include "hal/arch.hh"
#include "memlayout.hh"
#include "devs/spinlock.hh"
#include "buddysystem.hh"
#include "printer.hh"
#include "klib.hh"
#include "slab.hh"
#include "devs/dtb.hh"
#include "platform/memory.hh"
#include "libs/perf_diag.hh"
extern "C" char end[]; // 来自链接脚本
extern "C" char kernel_start[];
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

            // 除 DTB 与 initrd 外，实机固件还会通过 reservation map 和
            // /reserved-memory 声明显存、CMA、共享 DMA 等不可分配区域。
            constexpr int k_max_reserved_ranges = DtbManager::k_max_reserved_regions + 3;
            ReservedRange sorted[k_max_reserved_ranges]{};
            int count = reserved_count > k_max_reserved_ranges
                            ? k_max_reserved_ranges
                            : reserved_count;
            for (int index = 0; index < count; ++index)
            {
                sorted[index] = reserved[index];
            }
            for (int left = 0; left < count; ++left)
            {
                for (int right = left + 1; right < count; ++right)
                {
                    if (sorted[right].start < sorted[left].start)
                    {
                        const ReservedRange temporary = sorted[left];
                        sorted[left] = sorted[right];
                        sorted[right] = temporary;
                    }
                }
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
            // RISC-V 启动路径不会预先缓存 initrd；在扩大到 DTB 全量内存后必须
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

        // 固件保留区之外，还要容纳内核镜像、DTB 和 initrd 三个启动期区间。
        constexpr int k_max_reserved_ranges = DtbManager::k_max_reserved_regions + 3;
        ReservedRange reserved_ranges[k_max_reserved_ranges]{};
        int reserved_count = 0;

        const uint64 kernel_image_start = PGROUNDDOWN(
            platform::memory::physical_address(reinterpret_cast<uint64>(kernel_start)));
        const uint64 kernel_image_end = PGROUNDUP(
            platform::memory::physical_address(reinterpret_cast<uint64>(end)));
        if (kernel_image_end <= kernel_image_start)
        {
            panic("[pmm] invalid kernel image range: start=0x%lx end=0x%lx",
                  kernel_image_start, kernel_image_end);
        }
        // 把内核本身作为显式保留区，而不是只依靠“从 end 后开始分配”的
        // 隐含规则。这样重复、重叠或乱序的 DTB RAM 节点也无法重新释放内核页。
        reserved_ranges[reserved_count++] = {kernel_image_start, kernel_image_end};

        uint64 dtb_size = DtbManager::get_dtb_size();
        if (k_dtb_addr != 0)
        {
            if (dtb_size == 0)
            {
                panic("[pmm] validated DTB has no usable totalsize");
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

        DtbMemoryRegion firmware_reserved[DtbManager::k_max_reserved_regions]{};
        const int firmware_reserved_count = DtbManager::get_reserved_regions(
            firmware_reserved, DtbManager::k_max_reserved_regions);
        int accepted_firmware_reserved_count = 0;
        for (int index = 0;
             index < firmware_reserved_count && reserved_count < k_max_reserved_ranges;
             ++index)
        {
            uint64 reserved_end = 0;
            if (!checked_region_top(firmware_reserved[index].base,
                                    firmware_reserved[index].size,
                                    reserved_end))
            {
                boardPrintfWarn("[pmm] ignoring invalid reserved region[%d]\n", index);
                printfYellow("[pmm] ignoring invalid reserved region[%d]\n", index);
                continue;
            }
            reserved_ranges[reserved_count++] = {
                firmware_reserved[index].base, reserved_end};
            ++accepted_firmware_reserved_count;
        }

        DtbMemoryRegion regions[DtbManager::k_max_memory_regions]{};
        int region_count = DtbManager::get_memory_regions(regions, DtbManager::k_max_memory_regions);
        // 这些是 PMM 的硬件输入。逐项输出后，可以直接和 DTB 的 memory、
        // memreserve、reserved-memory、chosen/initrd 节点进行对照。boardPrintf
        // 会根据当前平台画像决定是否输出，因此内存管理器不需要认识具体板名。
        boardPrintfInfo("[pmm] input: ram-regions=%d firmware-reserved=%d/%d "
                        "combined-reserved=%d\n",
                        region_count, accepted_firmware_reserved_count,
                        firmware_reserved_count, reserved_count);
        for (int index = 0; index < region_count; ++index)
        {
            boardPrintf("[pmm] input ram[%d]=0x%lx-0x%lx size=%lu KiB\n",
                        index, regions[index].base,
                        regions[index].base + regions[index].size,
                        regions[index].size / 1024);
        }
        for (int index = 0; index < reserved_count; ++index)
        {
            boardPrintf("[pmm] input reserved[%d]=0x%lx-0x%lx\n",
                        index, reserved_ranges[index].start,
                        reserved_ranges[index].end);
        }
        printfGreen("[pmm] honored %d firmware reserved-memory ranges\n",
                    accepted_firmware_reserved_count);
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
                boardPrintfWarn("[pmm] ignoring invalid DTB memory region[%d]\n", i);
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

        if (kernel_region_index < 0)
        {
            panic("[pmm] DTB memory nodes do not contain the RISC-V kernel end=%p",
                  kernel_end_phys);
        }
        if (best_interval.bytes() == 0)
        {
            panic("[pmm] DTB has no usable RISC-V allocator interval");
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
                boardPrintfWarn("[pmm] ignoring invalid DTB memory region[%d]\n", i);
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
            // DTB 理论上不应重复或重叠描述 RAM，但实机固件并不总是规范。
            // 每一个覆盖内核末尾的节点都必须裁掉内核占用区，不能只裁最后
            // 一个匹配节点，否则重复节点会把正在运行的内核页再次交给 buddy。
            const bool contains_kernel =
                kernel_end_phys > regions[i].base && kernel_end_phys <= region_top;
            const uint64 region_start = contains_kernel
                                            ? kernel_end_phys
                                            : regions[i].base;
            UsableInterval candidate = largest_usable_interval(
                region_start, region_top, reserved_ranges, reserved_count);
            if (candidate.bytes() > best_interval.bytes())
            {
                best_interval = candidate;
            }
        }

        if (kernel_region_index < 0)
        {
            panic("[pmm] DTB memory nodes do not contain the LoongArch kernel end=0x%lx",
                  kernel_end_phys);
        }
        if (best_interval.bytes() == 0)
        {
            panic("[pmm] DTB has no usable LoongArch allocator interval");
        }

        // DMWIN 对所有 RAM 提供直映；因此页分配器使用 DTB 中最大的连续
        // 可用区，而不是永远困在包含内核的低端区。
        allocator_start = to_vir(best_interval.start);
        allocator_top = to_vir(best_interval.top);
#endif

        allocator_start = PGROUNDUP(allocator_start);
        allocator_top = PGROUNDDOWN(allocator_top);
        if (allocator_top <= allocator_start + PGSIZE * 64)
        {
            panic("[pmm] insufficient allocator region: start=%p top=%p",
                  allocator_start, allocator_top);
        }
        phys_top = allocator_top;
        boardPrintfInfo("[pmm] output: selected-ram=0x%lx-0x%lx size=%lu KiB\n",
                        platform::memory::physical_address(allocator_start),
                        platform::memory::physical_address(allocator_top),
                        (allocator_top - allocator_start) / 1024);

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
        // 输出的是物理地址而非 DMW 别名，便于与手册、U-Boot bdinfo 和 DTB
        // 使用同一坐标系核对。区间均为左闭右开 [start,end)。
        boardPrintfInfo("[pmm] output: buddy=0x%lx-0x%lx pages=%u "
                        "heap=0x%lx-0x%lx shm=0x%lx-0x%lx\n",
                        platform::memory::physical_address(pa_start),
                        platform::memory::physical_address(
                            pa_start + static_cast<uint64>(page_count) * PGSIZE),
                        page_count,
                        platform::memory::physical_address(heap_area_start),
                        platform::memory::physical_address(
                            heap_area_start + heap_allocator_size),
                        platform::memory::physical_address(shm_start),
                        platform::memory::physical_address(shm_start + shm_size));
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
        F7LY_PERF_ADD(PmmAllocPage, 1);
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
        F7LY_PERF_ADD(PmmAllocPage, count);
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
        F7LY_PERF_ADD(PmmReleaseRef, block.block_pages);
        F7LY_PERF_ADD(PmmFreePage, block.block_pages);
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
            F7LY_PERF_ADD(PmmReleaseRef, block.block_pages);
            F7LY_PERF_ADD(PmmFreePage, block.block_pages);
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
        F7LY_PERF_ADD(PmmReleaseRef, 1);
        if (should_free)
        {
            F7LY_PERF_ADD(PmmFreePage, 1);
        }
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
        F7LY_PERF_ADD(PmmReleaseRef, block.block_pages);
        F7LY_PERF_ADD(PmmFreePage, block.block_pages);
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

    void PhysicalMemoryManager::release_pages_batch(void *const *pages, uint32 count)
    {
        if (pages == nullptr || count == 0)
        {
            return;
        }

        uint32 released_refs = 0;
        uint32 freed_pages = 0;
        memlock.acquire();
        for (uint32 index = 0; index < count; ++index)
        {
            if (pages[index] == nullptr)
            {
                continue;
            }
            const uint64 page_index = pa2pgnm(pages[index]);
            if (!page_index_in_range(page_index))
            {
                // 与 free_page() 保持一致：坏地址不能污染 buddy，但单个异常项
                // 不应阻止同批其它合法页完成归还。
                printfRed("[pmm] release_pages_batch out of managed range: pa=%p index=%d pages=%d\n",
                          pages[index], static_cast<int>(page_index), page_count);
                continue;
            }

            BuddySystem::PageQueryResult block =
                _buddy->query_page(static_cast<uint32>(page_index));
            if (!block.in_range || block.is_free)
            {
                memlock.release();
                panic("[pmm] release_pages_batch on unallocated page pa=%p index=%lu",
                      pages[index], page_index);
            }
            if (block.block_pages != 1 || block.block_offset != page_index)
            {
                memlock.release();
                panic("[pmm] release_pages_batch requires single-page allocation pa=%p index=%lu block=%u+%u",
                      pages[index], page_index, block.block_offset, block.block_pages);
            }

            const uint16 refcount = k_page_refcounts[page_index];
            if (refcount == 0)
            {
                memlock.release();
                panic("[pmm] release_pages_batch found zero refcount pa=%p index=%lu",
                      pages[index], page_index);
            }
            ++released_refs;
            if (refcount > 1)
            {
                --k_page_refcounts[page_index];
                continue;
            }

            k_page_refcounts[page_index] = 0;
            _buddy->Free(static_cast<int>(page_index));
            ++freed_pages;
        }
        memlock.release();
        F7LY_PERF_ADD(PmmReleaseRef, released_refs);
        F7LY_PERF_ADD(PmmBatchReleaseRef, released_refs);
        F7LY_PERF_ADD(PmmFreePage, freed_pages);
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
            F7LY_PERF_ADD(PmmAllocPage, page_num);
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

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

    uint64 PhysicalMemoryManager::pa2pgnm(void *pa)
    {
        auto addr = reinterpret_cast<uint64>(pa);
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
        // 多核情况下应该加锁
        memlock.init("memlock");
        // 把原本Buddy的初始化放在这里，Buddy变成pmm的一个成员

        /*pa_start是buddy系统在物理内存中的起始地址,加上一个Sizeof(BuddySystem)后后面存的东西是tree,
        然后tree存完了之后才是buddy系统管理的那块内存。加上的BSSIZE是预留来放BuddySystem的大小和tree的大小，
        在这之后才是buddy系统管理的那块内存，这时pa_start指向的就是buddy系统管理的那块内存的开始地址，
        再被初始化为buddy的基址。*/
        pa_start = reinterpret_cast<uint64_t>(end);
        pa_start = (pa_start + PGSIZE - 1) & ~(PGSIZE - 1); // 将pa_start向高地址对齐到PGSIZE的整数倍
        _buddy = reinterpret_cast<BuddySystem *>(pa_start);
        pa_start += BSSIZE * PGSIZE;
        memset(_buddy, 0, BSSIZE * PGSIZE);
        uint64 heap_meta_bytes = BSSIZE * PGSIZE;
        uint64 min_heap_region = heap_meta_bytes + (_1M * 8);
        uint64 max_heap_region = heap_meta_bytes + vm_kernel_heap_size + SHM_SIZE;
        uint64 usable_top = PHYSTOP;
        bool use_split_heap_region = false;
#ifdef RISCV
        // 计算可用物理内存的上界，优先使用 dtb 位置作为上限以避免踩到 dtb
        if (k_dtb_addr && k_dtb_addr < usable_top)
        {
            usable_top = PGROUNDDOWN(k_dtb_addr);
        }
#elif defined(LOONGARCH)
        uint64 kernel_end_phys = VIRT2PHY(reinterpret_cast<uint64>(end));
        DtbMemoryRegion regions[DtbManager::k_max_memory_regions]{};
        int region_count = DtbManager::get_memory_regions(regions, DtbManager::k_max_memory_regions);
        int kernel_region_index = -1;
        int heap_region_index = -1;

        for (int i = 0; i < region_count; ++i)
        {
            uint64 region_base = regions[i].base;
            uint64 region_top = region_base + regions[i].size;
            printfGreen("[pmm] dtb memory region[%d]: base=%p size=%p top=%p\n",
                        i, region_base, regions[i].size, region_top);
            if (kernel_region_index < 0 &&
                kernel_end_phys >= region_base &&
                kernel_end_phys < region_top)
            {
                kernel_region_index = i;
            }
        }

        if (kernel_region_index >= 0)
        {
            kernel_linear_top = to_vir(regions[kernel_region_index].base + regions[kernel_region_index].size);
            usable_top = PGROUNDDOWN(kernel_linear_top);
        }
        else
        {
            kernel_linear_top = PGROUNDDOWN(PHYSTOP);
        }

        // LoongArch virt 的 1G 内存通常被拆成低端 RAM + 0x90000000 以上的高端 RAM。
        // 以前内核把整个物理空间强行当成一段连续区间，只能退化为 128MB 低端内存。
        // 这里保持页分配器继续使用“包含内核镜像的低端连续区”，
        // 同时把高端连续区整段拿来做 kernel heap/shm，避免把 PCI/内存空洞错误纳入 buddy。
        for (int i = region_count - 1; i >= 0; --i)
        {
            if (i == kernel_region_index)
            {
                continue;
            }
            if (regions[i].size >= min_heap_region)
            {
                heap_region_index = i;
                break;
            }
        }

        if (heap_region_index >= 0)
        {
            uint64 heap_region_phys_base = PGROUNDUP(regions[heap_region_index].base);
            uint64 heap_region_skip = heap_region_phys_base - regions[heap_region_index].base;
            if (regions[heap_region_index].size <= heap_region_skip)
            {
                panic("[pmm] split heap region alignment overflow");
            }
            uint64 heap_region_size = PGROUNDDOWN(regions[heap_region_index].size - heap_region_skip);
            if (heap_region_size > max_heap_region)
            {
                heap_region_size = max_heap_region;
            }
            if (heap_region_size < min_heap_region)
            {
                panic("[pmm] split heap region too small: %p", heap_region_size);
            }

            heap_area_start = to_vir(heap_region_phys_base);
            heap_area_size = heap_region_size;
            use_split_heap_region = true;
            printfGreen("[pmm] using split heap region: base=%p size=%p\n",
                        heap_area_start, heap_area_size);
        }
#endif

        phys_top = usable_top;
        if (kernel_linear_top == 0)
        {
            kernel_linear_top = usable_top;
        }

        if (usable_top <= pa_start + PGSIZE * 4)
        {
            panic("[pmm] insufficient memory: usable_top=%p, pa_start=%p", usable_top, pa_start);
        }

        uint64 available_bytes = usable_top - pa_start;
        if (available_bytes < PGSIZE * 16)
        {
            panic("[pmm] insufficient low memory for page allocator: %p", available_bytes);
        }

        if (!use_split_heap_region)
        {
            // 为堆和共享内存预留一部分空间，这里按 1/3 留给堆/共享内存，2/3 留给物理页分配
            uint64 heap_region_bytes = available_bytes / 3;
            if (heap_region_bytes < min_heap_region)
                heap_region_bytes = min_heap_region;
            if (heap_region_bytes > max_heap_region)
                heap_region_bytes = max_heap_region;
            if (heap_region_bytes + PGSIZE * 16 > available_bytes)
                heap_region_bytes = available_bytes > PGSIZE * 32 ? available_bytes - PGSIZE * 16 : available_bytes / 2;

            heap_area_start = PGROUNDDOWN(usable_top - heap_region_bytes);
            heap_area_size = usable_top - heap_area_start;
        }

        uint64 pmm_bytes = use_split_heap_region ? (usable_top - pa_start) : (heap_area_start - pa_start);
        if (pmm_bytes < PGSIZE * 16)
        {
            panic("[pmm] not enough space for page allocator: %p bytes", pmm_bytes);
        }

        page_count = pmm_bytes / PGSIZE;

        // 拆分堆区域：metadata + 普通堆 + 共享内存
        if (heap_area_size <= heap_meta_bytes)
        {
            panic("[pmm] heap region too small");
        }
        uint64 usable_heap_bytes = heap_area_size - heap_meta_bytes;
        uint64 tmp_shm_size = usable_heap_bytes / 3;
        if (tmp_shm_size > SHM_SIZE)
            tmp_shm_size = SHM_SIZE;
        heap_allocator_size = usable_heap_bytes - tmp_shm_size;
        if (heap_allocator_size > vm_kernel_heap_size)
        {
            heap_allocator_size = vm_kernel_heap_size;
            tmp_shm_size = usable_heap_bytes - heap_allocator_size;
        }
        heap_allocator_size = PGROUNDDOWN(heap_allocator_size);
        shm_size = PGROUNDDOWN(tmp_shm_size);
        if (heap_allocator_size + shm_size > usable_heap_bytes)
        {
            shm_size = usable_heap_bytes > heap_allocator_size ? usable_heap_bytes - heap_allocator_size : 0;
            shm_size = PGROUNDDOWN(shm_size);
        }
        shm_start = heap_area_start + heap_meta_bytes + heap_allocator_size;
        heap_page_count = heap_allocator_size / PGSIZE;

        if (heap_page_count == 0 || shm_size == 0)
        {
            panic("[pmm] heap/shm space too small (heap pages=%d, shm=%p)", heap_page_count, shm_size);
        }

        _buddy->Initialize(pa_start, page_count);
        printfGreen("[pmm] buddy system initialized, pa_start: %p, phys_top: %p, pages: %d\n",
                    pa_start, phys_top, page_count);
        printfGreen("[pmm] kernel linear top: %p\n", kernel_linear_top);
        printfGreen("[pmm] heap region: start=%p size=%p (usable=%p), heap_pages=%d\n",
                    heap_area_start, heap_area_size, heap_allocator_size, heap_page_count);
        printfGreen("[pmm] shm region: start=%p size=%p\n", shm_start, shm_size);
    }

    void *PhysicalMemoryManager::alloc_page()
    {

        int x = _buddy->Alloc(0);

        if (x == -1)
        {
            panic("[pmm] alloc_page failed");
        }
        void *pa = pgnm2pa(x);
        // printfCyan("分配物理页:  %p\n", pa);
        memset(pa, 0, PGSIZE);
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
        _buddy->Free(pa2pgnm(pa));
    }

    void PhysicalMemoryManager::free_page(void *pa)
    {
        // printfCyan("释放物理页:  %p\n", pa);
        _buddy->Free(pa2pgnm(pa));
    }
    void PhysicalMemoryManager::clear_page(void *pa)
    {
        uint64 *p = (uint64 *)pa;
        const uint cnt = PGSIZE >> 3;
        for (uint i = 0; i < cnt; i++)
            p[i] = 0;
    }

    void *PhysicalMemoryManager::kmalloc(size_t size)
    {
        int page_num = size_to_page_num(size);
        // printfCyan("kmalloc: size = %lu, page_num = %d\n", size, page_num);
        
        // 检查请求的页数是否合理
        if ((uint32)page_num > page_count) {
            printfRed("kmalloc: request too many pages (%d > %d)\n", page_num, page_count);
            return 0;
        }
        
        int x = _buddy->Alloc(page_num);
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
            // kmalloc() 可能一次拿到多页；这里需要把整段临时缓冲区都清零，
            // 否则后续按“已初始化内核缓冲”使用时会把旧数据带进系统调用语义里。
            memset(pa, 0, (size_t)page_num * PGSIZE);
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

    void *PhysicalMemoryManager::kcalloc(uint n, size_t size)
    {
        void *pa = kmalloc(n * size);
        if (pa == nullptr)
        {
            return nullptr;
        }
        memset(pa, 0, n * size);
        return pa;
    }

}

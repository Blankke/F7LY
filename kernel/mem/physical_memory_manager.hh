#pragma once
#include "types.hh"
#include "devs/spinlock.hh"
#include "buddysystem.hh"
#include "platform.hh"
namespace mem
{

    class PhysicalMemoryManager
    {
    public:
        struct PageDebugInfo
        {
            bool managed = false;
            bool aligned = false;
            uint64 page_offset = 0;
            uint64 page_pa = 0;
            BuddySystem::PageQueryResult buddy{};
        };

        static void init();
        static void *alloc_page(); // 分配单个物理页，失败表示内核不可恢复并 panic
        /**
         * @brief 分配一张已经清零的物理页，失败时返回空指针。
         *
         * 用户可见的匿名页、页表以及临时缓冲区都依赖这个清零契约；
         * 调用方不应再次 memset/clear_page，避免在缺页热路径重复刷 4 KiB。
         */
        static void *try_alloc_page();

        /**
         * @brief 分配一张未清零物理页，失败时返回空指针。
         *
         * 仅允许用于随后必定整页覆盖的路径，例如 COW 整页复制和完整文件页读入。
         * 在内容写满以前不得映射到用户空间，也不得把内容拷贝给用户。
         */
        static void *try_alloc_page_uninitialized();
        static void *alloc_pages(int count); // 分配连续多个物理页
        static void *try_alloc_pages(int count); // 尝试分配连续页，失败时返回空指针
        static void free_page(void *pa); // 释放单个物理页
        static void free_pages(void *pa); // 释放连续多个物理页
        static bool retain_page(void *pa); // 增加单页引用计数，用于 fork COW 共享
        /**
         * @brief 在一次 PMM 锁临界区内增加最多 64 张页的引用计数。
         *
         * 返回位图中的第 i 位表示 pages[i] 已成功 retain；失败项保持原引用
         * 不变。该接口用于 fork/COW 批处理，避免为每个 4K 页重复竞争全局锁。
         */
        static uint64 retain_pages_batch(void *const *pages, uint32 count);
        static uint16 page_ref_count(void *pa); // 查询单页引用计数
        static bool is_managed_page(void *pa); // 判断地址是否属于单页分配器管理范围
        static void free_page1(void *pa, uint64 size); // 释放单个物理页
        static void *kmalloc(size_t size); // 分配任意大小的内存块
        /**
         * @brief 分配未清零的连续内核缓冲区。
         *
         * 仅允许用于随后完整覆盖有效区间的 read/copy_in 临时缓冲；调用方
         * 必须只向用户复制实际完成的字节，不能暴露未初始化的页尾。
         */
        static void *kmalloc_uninitialized(size_t size);
        static void *kcalloc(uint n, size_t size);
        void clear_page(void *pa);
        static PageDebugInfo debug_query_page(void *pa);
        static uint64 get_phys_top() { return phys_top; }
        static uint64 get_kernel_linear_top() { return kernel_linear_top; }
        static uint64 get_heap_area_start() { return heap_area_start; }
        static uint64 get_heap_area_size() { return heap_area_size; }
        static uint64 get_heap_allocator_size() { return heap_allocator_size; }
        static uint64 get_shm_start() { return shm_start; }
        static uint64 get_shm_size() { return shm_size; }
        static uint32 get_page_count() { return page_count; }
        static uint64 get_free_page_count();
        static uint32 get_heap_page_count() { return heap_page_count; }

    private:
        static BuddySystem *_buddy;
        static uint64 pa_start;
        static class SpinLock memlock;
        static uint32 page_count;
        static uint32 heap_page_count;
        static uint64 phys_top;
        static uint64 kernel_linear_top;
        static uint64 heap_area_start;
        static uint64 heap_area_size;
        static uint64 heap_allocator_size;
        static uint64 shm_start;
        static uint64 shm_size;

        static uint64 pa2pgnm(void *pa);
        static void *pgnm2pa(int pgnm);
        static void *try_alloc_page_impl(bool clear);
        static void *kmalloc_impl(size_t size, bool clear);
        static int size_to_page_num(uint64 size);
    };
    extern PhysicalMemoryManager k_pmm;
}

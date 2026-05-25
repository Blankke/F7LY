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
        static void init();
        static void *alloc_page(); // 分配单个物理页
        static void *alloc_pages(int count); // 分配连续多个物理页
        static void free_page(void *pa); // 释放单个物理页
        static void free_pages(void *pa); // 释放连续多个物理页
        static void free_page1(void *pa, uint64 size); // 释放单个物理页
        static void *kmalloc(size_t size); // 分配任意大小的内存块
        static void *kcalloc(uint n, size_t size);
        void clear_page(void *pa);
        static uint64 get_phys_top() { return phys_top; }
        static uint64 get_kernel_linear_top() { return kernel_linear_top; }
        static uint64 get_heap_area_start() { return heap_area_start; }
        static uint64 get_heap_area_size() { return heap_area_size; }
        static uint64 get_heap_allocator_size() { return heap_allocator_size; }
        static uint64 get_shm_start() { return shm_start; }
        static uint64 get_shm_size() { return shm_size; }
        static uint32 get_page_count() { return page_count; }
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
        static int size_to_page_num(uint64 size);
    };
extern PhysicalMemoryManager k_pmm;
}

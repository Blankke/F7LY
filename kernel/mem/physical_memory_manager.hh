#pragma once
#include "types.hh"
#include "devs/spinlock.hh"

namespace mem
{

    class PhysicalMemoryManager
    {
    public:
        static void init();
        static void *alloc_page(); // 分配单个物理页
        static void free_page(void *pa);
        static void *kmalloc(size_t size); // 分配任意大小的内存块
        static void *kcalloc(uint n, size_t size);

    private:
        static uint64 pa_start;
        static class SpinLock memlock;

        static uint64 pa2pgnm(void *pa);
        static void *pgnm2pa(int pgnm);
        static int size_to_page_num(uint64 size);
    };

}
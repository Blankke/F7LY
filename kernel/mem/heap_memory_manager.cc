
#include "heap_memory_manager.hh"
#include "riscv/pagetable.hh"
#include "memlayout.hh"
#include "physical_memory_manager.hh"
namespace mem
{
    HeapMemoryManager k_hmm;

    void HeapMemoryManager::init(const char *lock_name)
    {
        heap_lock.init(lock_name);
    }

    void *HeapMemoryManager::allocate(uint64 size)
    {
        int64 size = (int64)size;
        if (size <= 0)
        {
            panic("L-allocator : Try to allocate 0 byte memory. Null will be returned.");
            return nullptr;
        }
        if (size >= (int64)(0x1UL << 20) * 64)
        {
            panic("L-allocator : request size too big : %d Bytes", size);
            return nullptr;
        }

        return PhysicalMemoryManager::kmalloc(size);
    }

    void HeapMemoryManager::free(void *p)
    {
        PhysicalMemoryManager::free_page(p);
    }
} // namespace mm

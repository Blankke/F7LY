#include "physical_memory_manager.hh"
#include "types.hh"
#include "platform.h"
#include "devs/spinlock.hh"
#include "buddysystem.hh"
#include "printer.hh"
#include "klib.hh"
#include "slab.hh"
namespace mem
{

    uint64 PhysicalMemoryManager::pa_start;
    SpinLock PhysicalMemoryManager::memlock;

    uint64 PhysicalMemoryManager::pa2pgnm(void *pa)
    {
        auto addr = reinterpret_cast<uint64>(pa);
        if (addr % PGSIZE != 0)
        {
            panic("kfree!");
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
        mem::BuddySystem::Initialize();
    }

    void *PhysicalMemoryManager::alloc_page()
    {
        int x = mem::BuddySystem::instance->Alloc(0);
        void *pa = pgnm2pa(x);
        memset(pa, 0, PGSIZE);
        return pa;
    }

    void PhysicalMemoryManager::free_page(void *pa)
    {
        mem::BuddySystem::instance->Free(pa2pgnm(pa));
    }

    void *PhysicalMemoryManager::kmalloc(size_t size)
    {
        if(size >= PGSIZE)
        {
            int x = mem::BuddySystem::instance->Alloc(size_to_page_num(size));
            void *pa = pgnm2pa(x);
            memset(pa, 0, PGSIZE);
            return pa;
        }
        else if(size < PGSIZE)
        {
            //there maybe some bugs to be fixed
            return SlabAllocator::alloc(size);
        }
    }

    void *PhysicalMemoryManager::kcalloc(uint n, size_t size)
    {
        kmalloc(n * size);
    }
}
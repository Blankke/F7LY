#pragma once
#include "spinlock.hh"


namespace mem
{
    class HeapMemoryManager
    {
    private:
        SpinLock heap_lock;

    public:
        HeapMemoryManager() {};
        void init(const char *lock_name);

        void *allocate(uint64 size);

        void free(void *p);
    };

    extern HeapMemoryManager k_hmm;
} // namespace mem


#pragma once

#include "spinlock.hh"

namespace proc
{
    class ShmManager;

    constexpr uint SHM_NUM = 10;
	constexpr uint MAX_SHM_PGNUM = 4;
    class Shm
    {
        friend ShmManager;
        
        private:
            SpinLock _lock;
            int refcnt;     // reference count
            int pagenum;    // number of pages
            void *phyaddr[ proc::MAX_SHM_PGNUM ];   // physical address

        public:
            Shm() {};
            void init(const char* _lock_name);
    };    
    
    extern Shm Shmtabs[proc::SHM_NUM ];
}
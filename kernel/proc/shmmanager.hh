
#pragma once
#include "proc/sharemem.hh"
#ifdef RISCV
#include "mem/riscv/pagetable.hh"
#elif defined(LoongArch)
#include "mem/loongarch/pagetable.hh"
#endif
namespace proc
{
    class ShmManager
    {   
        private:
            
        public:
            ShmManager() = default;
            void init(const char* _lock_name);
            int shmkeyused(uint key, uint mask);
            void *shmgetat(uint key,  uint num);
            int shmadd(uint key, uint pgnum, void *phyaddr[MAX_SHM_PGNUM]);
            void shmaddcnt(uint mask);
            int shmrefcnt(uint key);
            int shmrm(uint key);
            int shmrelease(mem::PageTable &pt, uint64 shm, uint keymask);
    };    

    extern ShmManager k_shmManager;
}
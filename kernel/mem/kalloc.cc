#include "kalloc.hh"
#include "types.hh"
#include "platform.h"
#include "devs/spinlock.hh"
#include "buddysystem.hh"
#include "printer.hh"
#include "klib.hh"


namespace mem {

uint64 KAlloc::pa_start;
SpinLock KAlloc::memlock;

uint64 KAlloc::pa2pgnm(void* pa) {
    auto addr = reinterpret_cast<uint64>(pa);
    if (addr % PGSIZE != 0) {
        panic("kfree!");
    }
    return (addr - pa_start) / PGSIZE;
}

void* KAlloc::pgnm2pa(int pgnm) {
    return reinterpret_cast<void*>(static_cast<uint64>(pgnm) * PGSIZE + pa_start);
}

int KAlloc::size_to_page_num(uint64 size) {
    return static_cast<int>(size / PGSIZE + (size % PGSIZE != 0));
}

void KAlloc::init() {
    //多核情况下应该加锁
    memlock.init("memlock");
    mem::BuddySystem::Initialize();
}

void* KAlloc::alloc() {
    int x = mem::BuddySystem::instance->Alloc(0);
    void* pa = pgnm2pa(x);
    memset(pa, 0, PGSIZE);
    return pa;
}

void KAlloc::free(void* pa) {
    mem::BuddySystem::instance->Free(pa2pgnm(pa));
}

void* KAlloc::kmalloc(uint64 size) {
    int x = mem::BuddySystem::instance->Alloc(size_to_page_num(size));
    void* pa = pgnm2pa(x);
    memset(pa, 0, PGSIZE);
    return pa;
}

void* KAlloc::kcalloc(uint n, uint64 size) {
    int x = mem::BuddySystem::instance->Alloc(size_to_page_num(n * size));
    void* pa = pgnm2pa(x);
    memset(pa, 0, PGSIZE);
    return pa;
}

}
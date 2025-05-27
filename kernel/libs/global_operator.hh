

#pragma once

#include <cstddef>
// #include <new>
#include "types.hh"

extern void *operator new(uint64 size);
extern void *operator new[](uint64 size);
extern void operator delete(void *p) noexcept;
extern void operator delete(void *p, uint64 size) noexcept;
extern void operator delete[](void *p) noexcept;
extern void operator delete[](void *p, uint64 size) noexcept;
// ===== 新增这一段：为 sized deallocation 提供实现 =====
extern void operator delete(void* ptr, std::size_t size, std::align_val_t align) ;
extern void operator delete[](void* ptr, std::size_t size, std::align_val_t align) ; 
extern void* operator new(std::size_t, void* p) noexcept;
extern void  operator delete(void*, void*) noexcept;


#pragma once

#include <cstddef>
#include <new>
#include "types.hh"

extern void *operator new(uint64 size);
extern void *operator new[](uint64 size);
extern void operator delete(void *p) noexcept;
extern void operator delete(void *p, uint64 size) noexcept;
extern void operator delete[](void *p) noexcept;
extern void operator delete[](void *p, uint64 size) noexcept;
// ===== 新增这一段：为 sized deallocation 提供实现 =====
 void operator delete(void* ptr, std::size_t size, std::align_val_t align) ;
 void operator delete[](void* ptr, std::size_t size, std::align_val_t align) ;
 

// inline void *operator new(size_t, void *p)     throw() { return p; }
// inline void *operator new[](size_t, void *p)   throw() { return p; }
// inline void  operator delete  (void *, void *) throw() { };
// inline void  operator delete[](void *, void *) throw() { };

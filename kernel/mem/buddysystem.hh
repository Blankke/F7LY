#pragma once
#include "types.hh"

#define PAGE_ORDER 10
#define PGNUM (1 << 19)
#define BSSIZE 320 //最多支持 2^15 = 32768 页的分配粒度

namespace mem {

enum NodeState {
    NODE_UNUSED = 0,
    NODE_USED = 1,
    NODE_SPLIT = 2,
    NODE_FULL = 3
};

class BuddySystem {
public:
    void Initialize(uint64 baseptr);
    int Alloc(int size);
    void Free(int offset);
    void* alloc_pages(int count);
    void free_pages(void* ptr);
    void* get_base_ptr() const { return base_ptr; }
private:

    BuddySystem() = default;
    int IndexOffset(int index, int level, int max_level) const;
    void MarkParent(int index);
    void Combine(int index);
    uint32 NextPowerOfTwo(uint32 x);

    // 内存管理相关
    constexpr uint64 AlignUp(uint64 addr, uint64 align);


    int level;
    uint8* tree;
    uint8* base_ptr;
};

} ;// namespace mem
#pragma once
#include "types.hh"

#define PAGE_ORDER 10
#define BSSIZE 320 

namespace mem {

enum NodeState {
    NODE_UNUSED = 0,
    NODE_USED = 1,
    NODE_SPLIT = 2,
    NODE_FULL = 3
};

class BuddySystem {
public:
    void Initialize(uint64 baseptr, uint32 total_pages);
    int Alloc(int size);
    void Free(int offset);
    void* alloc_pages(int count);
    void free_pages(void* ptr);
    void* get_base_ptr() const { return base_ptr; }
    uint32 get_page_count() const { return page_count; }
    uint32 get_max_free_block_pages() const;
    uint64 get_free_page_count() const;
private:

    BuddySystem() = default;
    uint32 NextPowerOfTwo(uint32 x);

    // 内存管理相关
    constexpr uint64 AlignUp(uint64 addr, uint64 align);

    void mark_unusable_leaves();
    void rebuild_parent_states();
    int node_depth(int index) const;
    int node_offset_pages(int index) const;
    uint32 node_block_pages(int depth) const;
    void split_unused_node(int index);
    uint8 summarize_children_state(int index) const;
    int allocate_from_node(int index, int depth, uint32 actual_pages);
    bool free_from_node(int index, int depth, int target_offset);
    uint32 max_free_block_pages_from_node(int index, uint32 block_pages) const;
    uint64 free_page_count_from_node(int index, uint32 block_pages) const;

    uint32 page_count;
    uint32 capacity_pages;
    int level;
    uint8* tree;
    uint8* base_ptr;
};

} ;// namespace mem

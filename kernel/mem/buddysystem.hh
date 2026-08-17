#pragma once
#include "types.hh"

#define PAGE_ORDER 10

namespace mem {

enum NodeState {
    NODE_UNUSED = 0,
    NODE_USED = 1,
    NODE_SPLIT = 2,
    NODE_FULL = 3
};

class BuddySystem {
public:
    struct PageQueryResult {
        bool in_range;
        bool is_free;
        uint8 node_state;
        uint32 block_pages;
        int node_index;
        int node_level;
        uint32 block_offset;
    };

    // BuddySystem 对象、tree 和受管内存彼此解耦。调用方必须显式提供 tree
    // 存储，避免内存容量增长后仍依赖固定的“base 前 320 页”隐式布局。
    static uint32 capacity_pages_for(uint32 total_pages);
    static uint64 required_tree_bytes(uint32 total_pages);
    static uint64 required_storage_bytes(uint32 total_pages);
    void Initialize(uint64 baseptr, uint32 total_pages,
                    void *tree_storage, uint64 tree_storage_bytes);
    int Alloc(int size);
    void Free(int offset);
    void* alloc_pages(int count);
    void free_pages(void* ptr);
    void* get_base_ptr() const { return base_ptr; }
    uint32 get_page_count() const { return page_count; }
    uint32 get_max_free_block_pages() const;
    uint64 get_free_page_count() const;
    PageQueryResult query_page(uint32 page_offset) const;
private:

    BuddySystem() = default;
    static uint32 NextPowerOfTwo(uint32 x);
    PageQueryResult query_page_from_node(int index, int level, uint32 block_offset,
                                         uint32 block_pages, uint32 page_offset) const;

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

    uint32 page_count;
    uint32 capacity_pages;
    // 所有 Alloc()/Free() 都在上层 allocator 锁内完成，直接维护实际空闲页数。
    // 读取方因此无需在大内存机器上递归扫描整棵 buddy 树。
    uint64 free_page_count_;
    int level;
    uint8* tree;
    uint8* base_ptr;
};

} ;// namespace mem

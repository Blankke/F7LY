// buddysystem.cc
#include "buddysystem.hh"
#include "types.hh"
#include "klib.hh"
#include "printer.hh"
#include "platform.hh"

namespace mem
{

    constexpr uint64 BuddySystem::AlignUp(uint64 addr, uint64 align)
    {
        return (addr + align - 1) & ~(align - 1);
    }

    uint32 BuddySystem::NextPowerOfTwo(uint32 x)
    {
        if (x == 0)
            return 1;
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        return x + 1;
    }

    void BuddySystem::Initialize(uint64 baseptr, uint32 total_pages)
    {
        // 初始化buddy系统，baseptr是buddy系统的起始地址
        // 原本的buddy是用来管理物理内存的，所以并没有初始化它管理的内存的操作
        // 这里解耦合，buddy同时用于管理pm和hm，这里的buddy初始化时不用初始化内存
        base_ptr = reinterpret_cast<uint8 *>(baseptr);
        page_count = total_pages ? total_pages : 1;
        printfGreen("[mem] Buddy System Init\n");
        // printf("[BuddySystem] base_ptr: %p\n", base_ptr);
        tree = base_ptr - BSSIZE * PGSIZE + sizeof(BuddySystem);
        level = 0;
        capacity_pages = NextPowerOfTwo(page_count);
        while ((1u << level) < capacity_pages)
            level++;
        // 计算所需的树节点数量
        int max_nodes = (1 << (level + 1)) - 1;
        int available_bytes = BSSIZE * PGSIZE - sizeof(BuddySystem);

        printf("[BuddySystem] level=%d, max_nodes=%d, available_bytes=%d\n",
               level, max_nodes, available_bytes);

        if (max_nodes > available_bytes)
        {
            panic("[BuddySystem] Tree size (%d bytes) exceeds available space (%d bytes)\n",
                  max_nodes, available_bytes);
        }

        // 初始化树数组
        for (int i = 0; i < max_nodes; i++)
        {
            tree[i] = NODE_UNUSED;
        }

        // 计算并打印实际管辖的内存区域
        uint64 managed_start = reinterpret_cast<uint64>(base_ptr);
        uint64 managed_end = managed_start + (static_cast<uint64>(capacity_pages) * PGSIZE);
        uint64 managed_size_mb = (static_cast<uint64>(capacity_pages) * PGSIZE) / (1024 * 1024);
        
        printf("[BuddySystem] Managed memory region: 0x%lx - 0x%lx (%lu MB, %d pages)\n",
               managed_start, managed_end, managed_size_mb, page_count);
        printf("[BuddySystem] Tree structure location: %p (size: %d bytes)\n", 
               tree, max_nodes);

        mark_unusable_leaves();
        rebuild_parent_states();

        printfGreen("[mem] Buddy System Init with %d pages (capacity %d), level=%d\n", page_count, capacity_pages, level);
    }

    void BuddySystem::mark_unusable_leaves()
    {
        int max_nodes = (1 << (level + 1)) - 1;
        int leaf_start = (1 << level) - 1;
        for (uint32 i = 0; i < capacity_pages; i++)
        {
            int idx = leaf_start + i;
            if (i >= page_count)
                tree[idx] = NODE_FULL;
            else
                tree[idx] = NODE_UNUSED;
        }
        for (int idx = leaf_start + capacity_pages; idx < max_nodes; ++idx)
        {
            tree[idx] = NODE_FULL;
        }
    }

    void BuddySystem::rebuild_parent_states()
    {
        int max_nodes = (1 << (level + 1)) - 1;
        for (int idx = (1 << level) - 2; idx >= 0; --idx)
        {
            int left = idx * 2 + 1;
            int right = left + 1;
            if (left >= max_nodes || right >= max_nodes)
            {
                tree[idx] = NODE_FULL;
                continue;
            }
            uint8 l = tree[left];
            uint8 r = tree[right];
            if (l == NODE_FULL && r == NODE_FULL)
                tree[idx] = NODE_FULL;
            else if (l == NODE_UNUSED && r == NODE_UNUSED)
                tree[idx] = NODE_UNUSED;
            else
                tree[idx] = NODE_SPLIT;
        }
    }

    int BuddySystem::node_depth(int index) const
    {
        int x = index + 1;
        int depth = 0;
        while ((1 << (depth + 1)) <= x)
        {
            ++depth;
        }
        return depth;
    }

    int BuddySystem::node_offset_pages(int index) const
    {
        const int depth = node_depth(index);
        const int pos = (index + 1) - (1 << depth);
        return pos << (level - depth);
    }

    uint32 BuddySystem::node_block_pages(int depth) const
    {
        return capacity_pages >> depth;
    }

    void BuddySystem::split_unused_node(int index)
    {
        if (tree[index] != NODE_UNUSED)
        {
            return;
        }

        tree[index] = NODE_SPLIT;
        tree[index * 2 + 1] = NODE_UNUSED;
        tree[index * 2 + 2] = NODE_UNUSED;
    }

    uint8 BuddySystem::summarize_children_state(int index) const
    {
        const int left = index * 2 + 1;
        const int right = left + 1;
        const uint8 l = tree[left];
        const uint8 r = tree[right];

        const bool left_unavailable = (l == NODE_USED || l == NODE_FULL);
        const bool right_unavailable = (r == NODE_USED || r == NODE_FULL);
        if (left_unavailable && right_unavailable)
        {
            return NODE_FULL;
        }
        if (l == NODE_UNUSED && r == NODE_UNUSED)
        {
            return NODE_UNUSED;
        }
        return NODE_SPLIT;
    }

    int BuddySystem::allocate_from_node(int index, int depth, uint32 actual_pages)
    {
        const uint32 block_pages = node_block_pages(depth);
        if (actual_pages > block_pages)
        {
            return -1;
        }

        const uint8 state = tree[index];
        if (state == NODE_USED || state == NODE_FULL)
        {
            return -1;
        }

        if (block_pages == actual_pages)
        {
            if (state != NODE_UNUSED)
            {
                return -1;
            }
            tree[index] = NODE_USED;
            return index;
        }

        if (state == NODE_UNUSED)
        {
            split_unused_node(index);
        }

        if (tree[index] != NODE_SPLIT)
        {
            return -1;
        }

        const int left = index * 2 + 1;
        int allocated = allocate_from_node(left, depth + 1, actual_pages);
        if (allocated == -1)
        {
            allocated = allocate_from_node(left + 1, depth + 1, actual_pages);
        }

        tree[index] = summarize_children_state(index);
        return allocated;
    }

    bool BuddySystem::free_from_node(int index, int depth, int target_offset)
    {
        const int offset = node_offset_pages(index);
        const uint32 block_pages = node_block_pages(depth);
        if (target_offset < offset || target_offset >= offset + static_cast<int>(block_pages))
        {
            return false;
        }

        if (tree[index] == NODE_USED)
        {
            if (offset != target_offset)
            {
                return false;
            }
            tree[index] = NODE_UNUSED;
            return true;
        }

        if (tree[index] == NODE_UNUSED)
        {
            return false;
        }

        const int left = index * 2 + 1;
        const int midpoint = offset + static_cast<int>(block_pages / 2);
        bool released = false;
        if (target_offset < midpoint)
        {
            released = free_from_node(left, depth + 1, target_offset);
        }
        else
        {
            released = free_from_node(left + 1, depth + 1, target_offset);
        }

        if (!released)
        {
            return false;
        }

        tree[index] = summarize_children_state(index);
        return true;
    }

    int BuddySystem::Alloc(int size)
    {
        // buddy 的最小分配单位是页；为了保证块对齐，内部按 2 的幂页数分配。
        const uint32 requested_pages = size <= 0 ? 1u : static_cast<uint32>(size);
        const uint32 actual_pages = NextPowerOfTwo(requested_pages);
        if (actual_pages > capacity_pages)
        {
            printfRed("[BuddySystem] Alloc failed, request too many pages\n");
            return -1;
        }
        const int node = allocate_from_node(0, 0, actual_pages);
        if (node < 0)
        {
            printfRed("[BuddySystem] Alloc failed, no suitable block found\n");
            return -1;
        }

        const int offset = node_offset_pages(node);
        if (offset < 0 || offset >= static_cast<int>(page_count))
        {
            panic("[BuddySystem] Alloc produced invalid offset=%d (page_count=%d)", offset, page_count);
        }
        return offset;
    }

    void BuddySystem::Free(int offset)
    {
        // buddy的单位是页，而不是页面大小，这个offset的意思是页的数量的偏移量
        // 这里需要把offset转换为页的偏移量，也就是offset*PGSIZE+base_ptr才是实际的内存地址

        if (offset < 0 || offset >= (int)page_count)
        {
            printfRed("[BuddySystem] Freeing invalid page offset=%d (page_count=%d)\n", offset, page_count);
            return;
        }
        if (!free_from_node(0, 0, offset))
        {
            printfRed("[BuddySystem] Freeing unknown page offset=%d\n", offset);
        }
    }

    void *BuddySystem::alloc_pages(int count)
    {
        // base_ptr points to the beginning of the managed region
        int offset = Alloc(count);
        if (offset == -1)
        {
            // printfRed("[BuddySystem]  request too many pages\n");
            return nullptr;
        }
        void *pa = reinterpret_cast<void *>(static_cast<uint64>(offset) * PGSIZE + base_ptr);
        memset(pa, 0, count * PGSIZE);
        return pa;
    }

    void BuddySystem::free_pages(void *ptr)
    {
        auto addr = reinterpret_cast<uint64>(ptr);
        if (addr % PGSIZE != 0)
        {
            panic("kfree!");
        }
        Free((addr - (uint64)base_ptr) / PGSIZE);
    }

    uint32 BuddySystem::max_free_block_pages_from_node(int index, uint32 block_pages) const
    {
        uint8 state = tree[index];
        if (state == NODE_UNUSED)
        {
            return block_pages;
        }
        if (state == NODE_USED || state == NODE_FULL || block_pages == 0)
        {
            return 0;
        }

        uint32 child_block_pages = block_pages / 2;
        uint32 left = max_free_block_pages_from_node(index * 2 + 1, child_block_pages);
        uint32 right = max_free_block_pages_from_node(index * 2 + 2, child_block_pages);
        return left > right ? left : right;
    }

    uint64 BuddySystem::free_page_count_from_node(int index, uint32 block_pages) const
    {
        uint8 state = tree[index];
        if (state == NODE_UNUSED)
        {
            return block_pages;
        }
        if (state == NODE_USED || state == NODE_FULL || block_pages == 0)
        {
            return 0;
        }

        uint32 child_block_pages = block_pages / 2;
        return free_page_count_from_node(index * 2 + 1, child_block_pages) +
               free_page_count_from_node(index * 2 + 2, child_block_pages);
    }

    uint32 BuddySystem::get_max_free_block_pages() const
    {
        return max_free_block_pages_from_node(0, capacity_pages);
    }

    uint64 BuddySystem::get_free_page_count() const
    {
        return free_page_count_from_node(0, capacity_pages);
    }
} // namespace mem

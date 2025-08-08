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

    void BuddySystem::Initialize(uint64 baseptr)
    {
        // 初始化buddy系统，baseptr是buddy系统的起始地址
        // 原本的buddy是用来管理物理内存的，所以并没有初始化它管理的内存的操作
        // 这里解耦合，buddy同时用于管理pm和hm，这里的buddy初始化时不用初始化内存
        base_ptr = reinterpret_cast<uint8 *>(baseptr);

        printfGreen("[mem] Buddy System Init\n");
        // printf("[BuddySystem] base_ptr: %p\n", base_ptr);
        tree = base_ptr - BSSIZE * PGSIZE + sizeof(BuddySystem);
        level = 0;
        while (!((1 << level) & PGNUM))
        {
            level++;
        }
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
        uint64 managed_end = managed_start + (static_cast<uint64>(PGNUM) * PGSIZE);
        uint64 managed_size_mb = (static_cast<uint64>(PGNUM) * PGSIZE) / (1024 * 1024);
        
        printf("[BuddySystem] Managed memory region: 0x%lx - 0x%lx (%lu MB, %d pages)\n",
               managed_start, managed_end, managed_size_mb, PGNUM);
        printf("[BuddySystem] Tree structure location: %p (size: %d bytes)\n", 
               tree, max_nodes);

        printfGreen("[mem] Buddy System Init with %d pages, level=%d\n", PGNUM, level);
    }

    int BuddySystem::IndexOffset(int index, int level, int max_level) const
    {
        // 计算该 index 在完全二叉树中的真实层级 L（根为 0）
        int x = index + 1; // 完全二叉堆性质：层级由 index+1 的最高位决定
        int L = 0;
        while ((1 << (L + 1)) <= x)
            ++L;

        // 若传入的 level 与实际层级不一致，给出提示，便于调试
        if (L != level)
        {
            // printfYellow("[BuddySystem] IndexOffset level mismatch: index=%d, passed_level=%d, computed_level=%d\n",
            //              index, level, L);
        }

        int pos = x - (1 << L);               // 该层内的位置
        int result = pos << (max_level - L);   // 映射到叶子层的起始页号
        return result;
    }

    void BuddySystem::MarkParent(int index)
    {
        while (true)
        {
            int buddy = index - 1 + (index & 1) * 2;
            if (buddy > 0 && (tree[buddy] == NODE_USED || tree[buddy] == NODE_FULL))
            {
                index = (index + 1) / 2 - 1;
                tree[index] = NODE_FULL;
            }
            else
            {
                return;
            }
        }
    }

    int BuddySystem::Alloc(int size)
    {
        // buddy的单位是页，而不是页面大小，这个size的意思是页的数量
        int actual_size = size == 0 ? 1 : NextPowerOfTwo(size);
        int length = 1 << level; // length变量表示当前结点对应的块的大小

        if (actual_size > length)
        {
            printfRed("[BuddySystem] Alloc failed, request too many pages\n");
            return -1;
        }
        int index = 0;
        int current_level = 0;

        while (index >= 0)
        {

            // if(current_level > - 30)
            // printf("[BuddySystem] index: %d, level: %d, current_level: %d,length: %d\n", index, level, current_level,length);
            if (actual_size == length)
            {

                if (tree[index] == NODE_UNUSED)
                {
                    tree[index] = NODE_USED;
                    MarkParent(index);
                    int result = IndexOffset(index, current_level, level);
                    // 添加调试信息
                    // printfYellow("[BuddySystem] Found block: index=%d, current_level=%d, level=%d, result=%d\n",
                    //              index, current_level, level, result);
                    return result;
                }
            }
            else
            {
                // printfBlue("%d", tree[index]);
                switch (tree[index])
                {
                case NODE_USED:
                case NODE_FULL:
                    break;
                case NODE_UNUSED:
                    // printfCyan("1");
                    tree[index] = NODE_SPLIT;
                    tree[index * 2 + 1] = NODE_UNUSED;
                    tree[index * 2 + 2] = NODE_UNUSED;
                    // printfCyan("2");
                    [[fallthrough]]; // 默认行为是向下执行
                default:
                    index = index * 2 + 1;
                    length /= 2;
                    current_level++;

                    continue;
                }
            }

            // if (index % 2 == 0) {
            //     ++index;
            //     continue;
            // }
            if (index & 1) // index是左孩子，变换到右孩子
            {
                ++index;
                continue;
            }

            for (;;)
            {
                current_level--;
                length *= 2;
                index = (index + 1) / 2 - 1; // 回溯到父节点
                if (index < 0)
                {
                    printfRed("[BuddySystem] Alloc failed, no suitable block found\n");
                    return -1; // 遍历完了没有找到合适的块，失败
                }
                if (index % 2 == 0)
                { // 当前是左孩子，变换到右孩子开始继续寻找。
                    ++index;
                    break;
                }
            }
        }
        // printfRed("[BuddySystem] Alloc failed, no suitable block found\n");
        return -1;
    }

    void BuddySystem::Combine(int index)
    {
        while (true)
        {
            int buddy = index - 1 + (index & 1) * 2;
            if (buddy < 0 || tree[buddy] != NODE_UNUSED)
            {
                tree[index] = NODE_UNUSED;
                while ((index = (index + 1) / 2 - 1) >= 0 && tree[index] == NODE_FULL)
                {
                    tree[index] = NODE_SPLIT;
                }
                return;
            }
            index = (index + 1) / 2 - 1;
        }
    }

    void BuddySystem::Free(int offset)
    {
        // buddy的单位是页，而不是页面大小，这个offset的意思是页的数量的偏移量
        // 这里需要把offset转换为页的偏移量，也就是offset*PGSIZE+base_ptr才是实际的内存地址
        int left = 0;
        int length = 1 << level; // 当前 index 所表示的块的大小（从整块开始）
        int index = 0;           // 从根节点开始遍历 buddy 树

        while (true)
        {

            // printf("[BuddySystem] Freeing page at address: %p, left: %d, length: %d,offset :%d\n", base_ptr + index * PGSIZE, left, length,offset);
            switch (tree[index])
            {
            case NODE_USED:
                Combine(index);
                return;
            case NODE_UNUSED:
                // panic("[BuddySystem] Freeing invalid page\n");
                return;
            default:
                length /= 2;
                if (offset < left + length)
                {
                    index = index * 2 + 1;
                }
                else
                {
                    left += length;
                    index = index * 2 + 2;
                }
                break;
            }
        }
    }

    void *BuddySystem::alloc_pages(int count)
    {
        // 这里base_ptr是buddy管理的内存的开始地址，alloc返回的是偏移量，
        // 这个偏移量就是相对基址的偏移量，所以这里需要加上基址才是实际的内存地址。
        // 我们包装的alloc_pages函数，返回的是实际的内存地址，
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
} // namespace mem
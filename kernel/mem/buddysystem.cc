// buddysystem.cc
#include "buddysystem.hh"
#include "types.hh"
#include "klib.hh"
#include "printer.hh"
#include "platform.hh"


namespace mem {




constexpr uint64 BuddySystem::AlignUp(uint64 addr, uint64 align) {
    return (addr + align - 1) & ~(align - 1);
}

uint32 BuddySystem::NextPowerOfTwo(uint32 x) {
    if (x == 0) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

void BuddySystem::Initialize(uint64 baseptr) {
    printfRed("init buddy system\n");  
    printf("[BuddySystem] base_ptr: %p\n", base_ptr);
    base_ptr = reinterpret_cast<uint8*>(baseptr);
    tree = base_ptr + sizeof(BuddySystem);
    level = 0;
    while (!((1 << level) & PGNUM)) {
        level++;
    }
}

int BuddySystem::IndexOffset(int index, int level, int max_level) const {
    return ((index + 1) - (1 << level)) << (max_level - level);
}

void BuddySystem::MarkParent(int index) {
    while (true) {
        int buddy = index - 1 + (index & 1) * 2;
        if (buddy > 0 && (tree[buddy] == NODE_USED || tree[buddy] == NODE_FULL)) {
            index = (index + 1) / 2 - 1;
            tree[index] = NODE_FULL;
        } else {
            return;
        }
    }
}

int BuddySystem::Alloc(int size) {
    int actual_size = size == 0 ? 1 : NextPowerOfTwo(size);
    int length = 1 << level;

    if (actual_size > length) return -1;

    int index = 0;
    int current_level = 0;

    while (index >= 0) {
        if (actual_size == length) {
            if (tree[index] == NODE_UNUSED) {
                tree[index] = NODE_USED;
                MarkParent(index);
                return IndexOffset(index, current_level, level);
            }
        } else {
            switch (tree[index]) {
                case NODE_USED:
                case NODE_FULL:
                    break;
                case NODE_UNUSED:
                    tree[index] = NODE_SPLIT;
                    tree[index * 2 + 1] = NODE_UNUSED;
                    tree[index * 2 + 2] = NODE_UNUSED;
                    [[fallthrough]];
                default:
                    index = index * 2 + 1;
                    length /= 2;
                    current_level++;
                    continue;
            }
        }

        if (index % 2 == 0) {
            ++index;
            continue;
        }

        for (;;) {
            current_level--;
            length *= 2;
            index = (index + 1) / 2 - 1;
            if (index < 0) return -1;
            if (index % 2 == 0) {
                ++index;
                break;
            }
        }
    }
    return -1;
}

void BuddySystem::Combine(int index) {
    while (true) {
        int buddy = index - 1 + (index & 1) * 2;
        if (buddy < 0 || tree[buddy] != NODE_UNUSED) {
            tree[index] = NODE_UNUSED;
            while ((index = (index + 1) / 2 - 1) >= 0 && tree[index] == NODE_FULL) {
                tree[index] = NODE_SPLIT;
            }
            return;
        }
        index = (index + 1) / 2 - 1;
    }
}

void BuddySystem::Free(int offset) {
    int left = 0;
    int length = 1 << level;
    int index = 0;

    while (true) {
        switch (tree[index]) {
            case NODE_USED:
                Combine(index);
                return;
            case NODE_UNUSED:
                printf("Freeing invalid page");
            default:
                length /= 2;
                if (offset < left + length) {
                    index = index * 2 + 1;
                } else {
                    left += length;
                    index = index * 2 + 2;
                }
                break;
        }
    }
}

void* BuddySystem::alloc_pages(int count) {
    int offset = Alloc(count);
    if (offset == -1)
    {
        printfRed("[BuddySystem]  request too many pages\n");
        return nullptr;
    }
    void* pa =  reinterpret_cast<void *>(static_cast<uint64>(offset) * PGSIZE + base_ptr);
    memset(pa, 0, count * PGSIZE);
    return pa;
}

void BuddySystem::free_pages(void* ptr) {
    auto addr = reinterpret_cast<uint64>(ptr);
    if (addr % PGSIZE != 0)
    {
        panic("kfree!");
    }
    Free((addr - (uint64)base_ptr) / PGSIZE);
}
} // namespace mem
// buddysystem.cc
#include "buddysystem.hh"
#include "types.hh"
#include "klib.hh"
#include "printer.hh"


extern "C" char end[]; // 来自链接脚本

namespace mem {

BuddySystem* BuddySystem::instance = nullptr;

BuddySystem::BuddySystem(uint64 base_addr) : base_ptr(reinterpret_cast<uint8*>(base_addr)) 
{
    tree = base_ptr + sizeof(BuddySystem);
}

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

void BuddySystem::Initialize() {

    const uint64_t PGSIZE = 1 << PAGE_ORDER;
    uint64_t pa_start = reinterpret_cast<uint64_t>(end);
    pa_start = (pa_start + PGSIZE - 1) & ~(PGSIZE - 1); //将pa_start向高地址对齐到PGSIZE的整数倍

    instance = reinterpret_cast<BuddySystem*>(pa_start);
    pa_start += BSSIZE * PGSIZE;
    memset(instance, 0, BSSIZE * PGSIZE);

    instance->level = 0;
    while (!((1 << instance->level) & PGNUM)) {
        instance->level++;
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

} // namespace mem
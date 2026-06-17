#pragma once

#include "types.hh"
#include "mem.hh"
#include "platform.hh"
#include <EASTL/unordered_map.h>

namespace fs
{
    class file;
}

namespace proc
{
    using VmPrivateOverlayMap = eastl::unordered_map<uint64, uint64>;

    class ProcessMemoryManager;
    class VmObject;

    enum VmaBackingKind : int
    {
        VMA_BACKING_NONE = 0,
        VMA_BACKING_FILE = 1,
        VMA_BACKING_SHM = 2,
    };

    enum class VmAreaKind : uint8
    {
        ElfLoad = 0,
        InterpreterLoad = 1,
        Heap = 2,
        UserStack = 3,
        Mmap = 4,
        SysvShm = 5,
    };

    enum class VmGrowPolicy : uint8
    {
        None = 0,
        Up = 1,
        Down = 2,
    };

    enum class VmAdviceState : uint8
    {
        None = 0,
        WipeOnFork = 1,
    };

    /**
     * @brief 统一的虚拟内存区域对象。
     *
     * 这里保留少量旧字段名，是为了让本轮大重构能分阶段落地；
     * 真正的权威后端状态已经转移到 object/page_offset/private_page_overlay。
     */
    struct VmArea
    {
        int used = 0;
        uint64 addr = 0;
        int len = 0;
        int prot = 0;
        int flags = 0;
        int vfd = -1;
        fs::file *vfile = nullptr;
        int offset = 0;
        uint64 max_len = 0;
        bool is_expandable = false;
        int backing_kind = VMA_BACKING_NONE;
        int backing_shmid = -1;
        uint64 backing_base = 0;
        bool has_resident_pages = false;
        bool wipe_on_fork = false;

        ProcessMemoryManager *owner_mm = nullptr;
        VmObject *object = nullptr;
        uint64 page_offset = 0;
        VmAreaKind area_kind = VmAreaKind::Mmap;
        VmGrowPolicy grow_policy = VmGrowPolicy::None;
        VmAdviceState advice_state = VmAdviceState::None;
        uint32 guard_pages = 0;
        bool zero_fill_past_file = false;
        uint64 file_backed_bytes = 0;
        const char *debug_name = nullptr;

        // 记录当前区域已经私有化/写入过的页。
        // 键为相对本 VMA 起点的页序号，值为当前页表里映射的物理页地址。
        VmPrivateOverlayMap *private_page_overlay = nullptr;

        uint64 end_addr() const
        {
            return addr + static_cast<uint64>(len);
        }

        bool valid_range() const
        {
            return used && len > 0 && end_addr() > addr;
        }

        bool covers(uint64 target) const
        {
            return valid_range() && target >= addr && target < end_addr();
        }

        bool overlaps(uint64 start, uint64 end) const
        {
            return valid_range() && start < end_addr() && end > addr;
        }

        bool overlaps(const VmArea &other) const
        {
            return overlaps(other.addr, other.end_addr());
        }

        bool is_private_mapping() const
        {
            if (area_kind == VmAreaKind::Heap || area_kind == VmAreaKind::UserStack)
            {
                return true;
            }
            return (flags & MAP_PRIVATE) != 0;
        }

        bool is_shared_mapping() const
        {
            return area_kind == VmAreaKind::SysvShm || (flags & MAP_SHARED) != 0;
        }

        bool grows_down() const
        {
            return grow_policy == VmGrowPolicy::Down;
        }

        bool grows_up() const
        {
            return grow_policy == VmGrowPolicy::Up;
        }

        uint64 page_index_for_va(uint64 va) const
        {
            return (PGROUNDDOWN(va) - PGROUNDDOWN(addr)) / PGSIZE;
        }

        uint64 mapped_page_offset(uint64 va) const
        {
            return page_offset + page_index_for_va(va) * PGSIZE;
        }
    };

    using vma = VmArea;
}

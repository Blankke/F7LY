#include "vma_space.hh"

#include "process_memory_manager.hh"
#include "printer.hh"
#include "vma_metadata_utils.hh"
#include "vm_object.hh"

namespace proc
{
    VMASpace::VMASpace() : owner_mm_(nullptr), mmap_cursor_(0) {}

    void VMASpace::init(ProcessMemoryManager *owner)
    {
        owner_mm_ = owner;
        mmap_cursor_ = 0;
        areas_.clear();
        index_.clear();
    }

    void VMASpace::clear()
    {
        for (auto &area : areas_)
        {
            vma_meta::release_metadata(area);
        }
        areas_.clear();
        index_.clear();
    }

    vma *VMASpace::create_area(uint64 addr,
                               uint64 length,
                               int prot,
                               int flags,
                               VmObject *object,
                               uint64 page_offset,
                               VmAreaKind kind,
                               VmGrowPolicy grow_policy,
                               uint32 guard_pages,
                               const char *debug_name)
    {
        if (length == 0)
        {
            return nullptr;
        }

        vma area = {};
        area.used = 1;
        area.addr = addr;
        area.len = length;
        area.prot = prot;
        area.flags = flags;
        area.owner_mm = owner_mm_;
        area.object = object;
        area.page_offset = page_offset;
        area.area_kind = kind;
        area.grow_policy = grow_policy;
        area.guard_pages = guard_pages;
        area.debug_name = debug_name;
        area.max_len = length;
        area.is_expandable = grow_policy != VmGrowPolicy::None;
        area.wipe_on_fork = false;

        areas_.push_back(area);
        vma &inserted = areas_.back();
        if (!insert_area(inserted))
        {
            if (inserted.object != nullptr && inserted.object->put())
            {
                delete inserted.object;
            }
            areas_.pop_back();
            return nullptr;
        }
        return &inserted;
    }

    vma *VMASpace::clone_area_from(const vma &src)
    {
        areas_.emplace_back();
        vma &inserted = areas_.back();
        if (!vma_meta::clone_snapshot(inserted, src))
        {
            areas_.pop_back();
            return nullptr;
        }

        inserted.owner_mm = owner_mm_;
        if (inserted.valid_range() && !insert_area(inserted))
        {
            vma_meta::release_metadata(inserted);
            areas_.pop_back();
            return nullptr;
        }
        return &inserted;
    }

    void VMASpace::destroy_area(vma *area)
    {
        if (area == nullptr)
        {
            return;
        }

        erase_area(*area, area->addr);
        for (auto it = areas_.begin(); it != areas_.end(); ++it)
        {
            if (&(*it) != area)
            {
                continue;
            }
            vma_meta::release_metadata(*it);
            areas_.erase(it);
            return;
        }
    }

    bool VMASpace::rebuild_index()
    {
        index_.clear();
        for (auto &area : areas_)
        {
            if (!area.valid_range())
            {
                continue;
            }
            if (!index_.insert(&area))
            {
                return false;
            }
        }
        return true;
    }

    bool VMASpace::insert_area(vma &area)
    {
        return area.valid_range() && index_.insert(&area);
    }

    void VMASpace::erase_area(vma &area, uint64 old_addr)
    {
        index_.erase(&area, old_addr == 0 ? area.addr : old_addr);
    }

    bool VMASpace::reindex_area(vma &area, uint64 old_addr)
    {
        index_.erase(&area, old_addr);
        if (index_.insert(&area))
        {
            return true;
        }
        return rebuild_index();
    }

    vma *VMASpace::find_vma_covering(uint64 addr)
    {
        return index_.find(addr);
    }

    const vma *VMASpace::find_vma_covering(uint64 addr) const
    {
        return index_.find(addr);
    }

    vma *VMASpace::find_first_vma_at_or_after(uint64 addr)
    {
        return index_.lower_bound(addr);
    }

    const vma *VMASpace::find_first_vma_at_or_after(uint64 addr) const
    {
        return index_.lower_bound(addr);
    }

    vma *VMASpace::find_prev_vma(uint64 start_addr)
    {
        return index_.prev_by_start(start_addr);
    }

    const vma *VMASpace::find_prev_vma(uint64 start_addr) const
    {
        return index_.prev_by_start(start_addr);
    }

    vma *VMASpace::find_next_vma(const vma *entry)
    {
        return index_.next(entry);
    }

    const vma *VMASpace::find_next_vma(const vma *entry) const
    {
        return index_.next(entry);
    }

    bool VMASpace::can_coalesce_private_anonymous(const vma &left, const vma &right) const
    {
        if (!left.valid_range() || !right.valid_range() ||
            left.end_addr() != right.addr ||
            left.len > UINT64_MAX - right.len ||
            left.owner_mm != owner_mm_ || right.owner_mm != owner_mm_)
        {
            return false;
        }

        /*
         * 大地址空间分配器会先保留大块 PROT_NONE 区域，再以 4 KiB 粒度
         * mprotect 为可读写。这里必须像 Linux 一样把权限相同的相邻片段
         * 重新合并，否则长时间运行会制造上万个 VMA。共享、文件和
         * overlay 映射具有额外的引用/偏移所有权，留给各自后端处理。
         */
        if ((left.flags & MAP_PRIVATE) == 0 || (left.flags & MAP_ANONYMOUS) == 0 ||
            left.vfd != -1 || right.vfd != -1 ||
            left.vfile != nullptr || right.vfile != nullptr ||
            left.object != nullptr || right.object != nullptr ||
            left.private_page_overlay != nullptr || right.private_page_overlay != nullptr ||
            left.backing_kind != VMA_BACKING_NONE || right.backing_kind != VMA_BACKING_NONE)
        {
            return false;
        }

        return left.prot == right.prot &&
               left.flags == right.flags &&
               left.area_kind == right.area_kind &&
               left.grow_policy == right.grow_policy &&
               left.advice_state == right.advice_state &&
               left.guard_pages == right.guard_pages &&
               left.wipe_on_fork == right.wipe_on_fork &&
               left.zero_fill_past_file == right.zero_fill_past_file &&
               left.debug_name == right.debug_name &&
            left.max_len == right.max_len &&
            left.is_expandable == right.is_expandable &&
            left.file_backed_bytes == 0 && right.file_backed_bytes == 0 &&
            left.page_offset <= UINT64_MAX - left.len &&
            left.offset <= UINT64_MAX - left.len &&
            left.page_offset + left.len == right.page_offset &&
            left.offset + left.len == right.offset;
    }

    bool VMASpace::merge_private_anonymous(vma &left, vma &right)
    {
        if (!can_coalesce_private_anonymous(left, right))
        {
            return false;
        }

        const uint64 old_len = left.len;
        const bool old_has_resident_pages = left.has_resident_pages;
        erase_area(left, left.addr);
        erase_area(right, right.addr);

        left.len += right.len;
        left.has_resident_pages = left.has_resident_pages || right.has_resident_pages;
        if (!insert_area(left))
        {
            left.len = old_len;
            left.has_resident_pages = old_has_resident_pages;
            // 回滚时两段仍互不重叠；任一重插失败都表示索引已经不可恢复。
            if (!insert_area(left) || !insert_area(right))
            {
                panic("VMASpace: failed to rollback anonymous VMA merge");
            }
            return false;
        }

        // 上面的严格条件保证 right 不持有需要转移的后端元数据。
        destroy_area(&right);
        return true;
    }

    vma *VMASpace::coalesce_private_anonymous_around(vma *area)
    {
        if (area == nullptr)
        {
            return nullptr;
        }

        vma *previous = find_prev_vma(area->addr);
        if (previous != nullptr && merge_private_anonymous(*previous, *area))
        {
            area = previous;
        }

        while (true)
        {
            vma *next_area = find_next_vma(area);
            if (next_area == nullptr || !merge_private_anonymous(*area, *next_area))
            {
                break;
            }
        }
        return area;
    }

    void VMASpace::coalesce_private_anonymous_range(uint64 start_addr, uint64 end_addr)
    {
        if (end_addr <= start_addr)
        {
            return;
        }

        vma *area = find_vma_covering(start_addr);
        if (area == nullptr)
        {
            area = find_first_vma_at_or_after(start_addr);
        }

        while (area != nullptr && area->addr < end_addr)
        {
            area = coalesce_private_anonymous_around(area);
            vma *next_area = find_next_vma(area);
            if (next_area == nullptr || next_area->addr >= end_addr)
            {
                break;
            }
            area = next_area;
        }
    }

    bool VMASpace::has_conflict(uint64 start_addr, uint64 end_addr, const vma *ignore) const
    {
        return index_.has_conflict(start_addr, end_addr, ignore);
    }

    uint64 VMASpace::find_gap(uint64 start_hint,
                              uint64 min_addr,
                              uint64 max_addr,
                              uint64 size,
                              uint64 alignment) const
    {
        return index_.find_gap(start_hint, min_addr, max_addr, size, alignment);
    }

    vma *VMASpace::find_heap_area()
    {
        for (auto &area : areas_)
        {
            if (area.area_kind == VmAreaKind::Heap)
            {
                return &area;
            }
        }
        return nullptr;
    }

    const vma *VMASpace::find_heap_area() const
    {
        for (const auto &area : areas_)
        {
            if (area.area_kind == VmAreaKind::Heap)
            {
                return &area;
            }
        }
        return nullptr;
    }

    vma *VMASpace::find_stack_area()
    {
        for (auto &area : areas_)
        {
            if (area.area_kind == VmAreaKind::UserStack)
            {
                return &area;
            }
        }
        return nullptr;
    }

    const vma *VMASpace::find_stack_area() const
    {
        for (const auto &area : areas_)
        {
            if (area.area_kind == VmAreaKind::UserStack)
            {
                return &area;
            }
        }
        return nullptr;
    }
}

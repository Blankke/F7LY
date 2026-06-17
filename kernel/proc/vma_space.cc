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
            if (area.object != nullptr)
            {
                area.object->on_area_destroy(area);
                if (area.object->put())
                {
                    delete area.object;
                }
                area.object = nullptr;
            }
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
        area.len = static_cast<int>(length);
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
            if (it->object != nullptr)
            {
                it->object->on_area_destroy(*it);
                if (it->object->put())
                {
                    delete it->object;
                }
                it->object = nullptr;
            }
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

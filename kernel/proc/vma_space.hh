#pragma once

#include "vm_area.hh"
#include "vma_maple_tree.hh"
#include <EASTL/list.h>

namespace proc
{
    class ProcessMemoryManager;
    class VmObject;

    class VMASpace
    {
    public:
        VMASpace();

        void init(ProcessMemoryManager *owner);
        void clear();

        vma *create_area(uint64 addr,
                         uint64 length,
                         int prot,
                         int flags,
                         VmObject *object,
                         uint64 page_offset,
                         VmAreaKind kind,
                         VmGrowPolicy grow_policy,
                         uint32 guard_pages,
                         const char *debug_name);
        vma *clone_area_from(const vma &src);
        void destroy_area(vma *area);

        bool rebuild_index();
        bool insert_area(vma &area);
        void erase_area(vma &area, uint64 old_addr = 0);
        bool reindex_area(vma &area, uint64 old_addr);

        vma *find_vma_covering(uint64 addr);
        const vma *find_vma_covering(uint64 addr) const;
        vma *find_first_vma_at_or_after(uint64 addr);
        const vma *find_first_vma_at_or_after(uint64 addr) const;
        vma *find_prev_vma(uint64 start_addr);
        const vma *find_prev_vma(uint64 start_addr) const;
        vma *find_next_vma(const vma *entry);
        const vma *find_next_vma(const vma *entry) const;

        /**
         * @brief 合并范围内由 mprotect 拆出的相邻私有匿名 VMA。
         *
         * 仅合并没有文件、VmObject 和私有 overlay 的轻量匿名映射，避免
         * 改变共享映射或文件偏移语义。调用方必须持有地址空间内存锁。
         */
        void coalesce_private_anonymous_range(uint64 start_addr, uint64 end_addr);

        bool has_conflict(uint64 start_addr, uint64 end_addr, const vma *ignore = nullptr) const;
        uint64 find_gap(uint64 start_hint, uint64 min_addr, uint64 max_addr, uint64 size, uint64 alignment) const;

        vma *find_heap_area();
        const vma *find_heap_area() const;
        vma *find_stack_area();
        const vma *find_stack_area() const;

        uint64 mmap_cursor() const { return mmap_cursor_; }
        void set_mmap_cursor(uint64 cursor) { mmap_cursor_ = cursor; }
        ProcessMemoryManager *owner_mm() { return owner_mm_; }
        const ProcessMemoryManager *owner_mm() const { return owner_mm_; }

        template <typename Fn>
        bool for_each(Fn &&fn)
        {
            return index_.for_each(static_cast<Fn &&>(fn));
        }

        template <typename Fn>
        bool for_each(Fn &&fn) const
        {
            return index_.for_each(static_cast<Fn &&>(fn));
        }

        template <typename Fn>
        bool for_each_in_range(uint64 start, uint64 end, Fn &&fn)
        {
            return index_.for_each_in_range(start, end, static_cast<Fn &&>(fn));
        }

        template <typename Fn>
        bool for_each_in_range(uint64 start, uint64 end, Fn &&fn) const
        {
            return index_.for_each_in_range(start, end, static_cast<Fn &&>(fn));
        }

    private:
        bool can_coalesce_private_anonymous(const vma &left, const vma &right) const;
        bool merge_private_anonymous(vma &left, vma &right);
        vma *coalesce_private_anonymous_around(vma *area);

        ProcessMemoryManager *owner_mm_;
        eastl::list<vma> areas_;
        VmaMapleTree index_;
        uint64 mmap_cursor_;
    };
}

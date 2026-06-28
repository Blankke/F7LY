#include "vm_object.hh"

#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "fs/vfs/file/file.hh"
#include "fs/vfs/virtual_fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "printer.hh"
#include "proc/signal.hh"
#include "proc_manager.hh"
#include "shm/shm_manager.hh"

namespace proc
{
    namespace
    {
        eastl::atomic<uint64> g_next_vm_object_id{1};

        class SpinLockGuard
        {
        public:
            explicit SpinLockGuard(SpinLock &lock) : lock_(lock)
            {
                lock_.acquire();
            }

            ~SpinLockGuard()
            {
                lock_.release();
            }

            SpinLockGuard(const SpinLockGuard &) = delete;
            SpinLockGuard &operator=(const SpinLockGuard &) = delete;

        private:
            SpinLock &lock_;
        };

        inline void *page_pa_to_kernel_ptr(uint64 pa)
        {
#ifdef LOONGARCH
            return reinterpret_cast<void *>(to_vir(pa));
#else
            return reinterpret_cast<void *>(pa);
#endif
        }

        inline bool retain_page_for_mapping(uint64 pa)
        {
            return mem::k_pmm.retain_page(page_pa_to_kernel_ptr(pa));
        }

        inline void fill_zero_page(void *page)
        {
            mem::k_pmm.clear_page(page);
        }

        inline int signal_sigbus_for_current_task(VmPageView &view)
        {
            Pcb *p = proc::k_pm.get_cur_pcb();
            if (p == nullptr)
            {
                return -1;
            }
            proc::ipc::signal::add_signal(p, proc::ipc::signal::SIGBUS);
            view.signal_delivered = true;
            return 0;
        }
    }

    VmObject::VmObject(VmObjectKind kind, bool shared_mapping)
        : object_id_(g_next_vm_object_id.fetch_add(1, eastl::memory_order_relaxed)),
          object_kind_(kind),
          shared_mapping_(shared_mapping),
          ref_count_(1)
    {
        object_lock_.init("vm_object");
        // 只对共享对象做全局登记，私有匿名对象/普通私有映射不必占用这把全局锁。
        // 线程栈和大多数 mmap 私有映射都落在这里，避免每次创建都去碰共享对象表。
        if (shared_mapping_)
        {
            shm::k_smm.note_object_created(this);
        }
    }

    VmObject::~VmObject()
    {
        release_source_pages();
        if (shared_mapping_)
        {
            shm::k_smm.note_object_destroying(this);
        }
    }

    void VmObject::get()
    {
        ref_count_.fetch_add(1, eastl::memory_order_relaxed);
    }

    bool VmObject::put()
    {
        return ref_count_.fetch_sub(1, eastl::memory_order_acq_rel) == 1;
    }

    int VmObject::ref_count_for_debug() const
    {
        return ref_count_.load(eastl::memory_order_acquire);
    }

    uint64 VmObject::ensure_source_page(uint64 key, bool zero_fill)
    {
        auto found = source_pages_.find(key);
        if (found != source_pages_.end())
        {
            return found->second;
        }

        void *page = mem::PhysicalMemoryManager::try_alloc_page();
        if (page == nullptr)
        {
            return 0;
        }
        if (zero_fill)
        {
            fill_zero_page(page);
        }
        source_pages_[key] = reinterpret_cast<uint64>(page);
        return reinterpret_cast<uint64>(page);
    }

    uint64 VmObject::find_source_page(uint64 key) const
    {
        auto found = source_pages_.find(key);
        return found == source_pages_.end() ? 0 : found->second;
    }

    uint64 VmObject::allocate_private_overlay_page(VmArea &area,
                                                   uint64 page_index,
                                                   const void *src,
                                                   size_t copy_bytes,
                                                   bool zero_fill_tail)
    {
        void *page = mem::PhysicalMemoryManager::try_alloc_page();
        if (page == nullptr)
        {
            return 0;
        }

        if (src != nullptr && copy_bytes != 0)
        {
            memmove(page, src, copy_bytes);
            if (zero_fill_tail && copy_bytes < PGSIZE)
            {
                memset(reinterpret_cast<char *>(page) + copy_bytes, 0, PGSIZE - copy_bytes);
            }
        }
        else
        {
            fill_zero_page(page);
        }

        if (area.private_page_overlay == nullptr)
        {
            area.private_page_overlay = new VmPrivateOverlayMap();
            if (area.private_page_overlay == nullptr)
            {
                mem::k_pmm.free_page(page);
                return 0;
            }
        }

        (*area.private_page_overlay)[page_index] = reinterpret_cast<uint64>(page);
        return reinterpret_cast<uint64>(page);
    }

    void VmObject::release_source_pages()
    {
        for (auto &entry : source_pages_)
        {
            if (entry.second != 0)
            {
                mem::k_pmm.free_page(page_pa_to_kernel_ptr(entry.second));
            }
        }
        source_pages_.clear();
    }

    void VmObject::on_area_destroy(VmArea &area)
    {
        if (area.private_page_overlay == nullptr)
        {
            return;
        }

        for (auto &entry : *area.private_page_overlay)
        {
            if (entry.second != 0)
            {
                mem::k_pmm.free_page(page_pa_to_kernel_ptr(entry.second));
            }
        }
        delete area.private_page_overlay;
        area.private_page_overlay = nullptr;
    }

    AnonVmObject::AnonVmObject(bool shared_mapping, const char *debug_name)
        : VmObject(VmObjectKind::Anon, shared_mapping), debug_name_(debug_name)
    {
        (void)debug_name_;
    }

    AnonVmObject::~AnonVmObject() = default;

    int AnonVmObject::prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view)
    {
        if (area.private_page_overlay != nullptr)
        {
            auto overlay = area.private_page_overlay->find(page_index);
            if (overlay != area.private_page_overlay->end())
            {
                view.pa = overlay->second;
                view.writable = (area.prot & PROT_WRITE) != 0;
                view.private_overlay = true;
                return retain_page_for_mapping(view.pa) ? 0 : -1;
            }
        }

        if (area.is_private_mapping() && access_type == 1)
        {
            uint64 overlay_pa = allocate_private_overlay_page(area, page_index, nullptr, 0, false);
            if (overlay_pa == 0)
            {
                return -1;
            }
            view.pa = overlay_pa;
            view.writable = true;
            view.private_overlay = true;
            return retain_page_for_mapping(view.pa) ? 0 : -1;
        }

        SpinLockGuard guard(object_lock_);
        uint64 source_pa = ensure_source_page(page_index, true);
        if (source_pa == 0)
        {
            return -1;
        }

        view.pa = source_pa;
        view.writable = shared_mapping() && (area.prot & PROT_WRITE) != 0;
        view.mark_cow = !shared_mapping() && (area.prot & PROT_WRITE) != 0;
        return retain_page_for_mapping(view.pa) ? 0 : -1;
    }

    FileVmObject::FileVmObject(fs::file *file,
                               bool shared_mapping,
                               bool zero_fill_past_file,
                               const eastl::string &cache_key)
        : VmObject(VmObjectKind::File, shared_mapping),
          file_(file),
          zero_fill_past_file_(zero_fill_past_file),
          cache_key_(cache_key)
    {
        if (file_ != nullptr)
        {
            file_->dup();
        }
    }

    FileVmObject::~FileVmObject()
    {
        if (file_ != nullptr)
        {
            file_->free_file();
            file_ = nullptr;
        }
    }

    int FileVmObject::prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view)
    {
        if (area.private_page_overlay != nullptr)
        {
            auto overlay = area.private_page_overlay->find(page_index);
            if (overlay != area.private_page_overlay->end())
            {
                view.pa = overlay->second;
                view.writable = (area.prot & PROT_WRITE) != 0;
                view.private_overlay = true;
                return retain_page_for_mapping(view.pa) ? 0 : -1;
            }
        }

        uint64 object_page_index = (area.page_offset / PGSIZE) + page_index;
        uint64 file_offset = area.page_offset + page_index * PGSIZE;
        uint64 page_offset_in_area = page_index * PGSIZE;
        uint64 file_backed_bytes = area.file_backed_bytes;
        if (file_ != nullptr &&
            area.area_kind == VmAreaKind::Mmap &&
            !area.zero_fill_past_file)
        {
            fs::Kstat st = {};
            int stat_ret = fs::k_vfs.fstat(file_, &st);
            if (stat_ret == EOK)
            {
                uint64 area_len = static_cast<uint64>(area.len);
                if (area.page_offset < st.size)
                {
                    uint64 bytes_left = st.size - area.page_offset;
                    file_backed_bytes = bytes_left > area_len ? area_len : bytes_left;
                }
                else
                {
                    file_backed_bytes = 0;
                }
                area.file_backed_bytes = file_backed_bytes;
            }
        }
        uint64 source_pa = 0;
        {
            SpinLockGuard guard(object_lock_);
            source_pa = find_source_page(object_page_index);
        }
        if (source_pa == 0)
        {
            if (file_ == nullptr)
            {
                return -1;
            }

            if (file_backed_bytes != 0 && page_offset_in_area < file_backed_bytes)
            {
                uint64 bytes_remaining = file_backed_bytes - page_offset_in_area;
                size_t bytes_to_read = bytes_remaining > PGSIZE ? PGSIZE : static_cast<size_t>(bytes_remaining);
                void *page = mem::PhysicalMemoryManager::try_alloc_page();
                if (page == nullptr)
                {
                    return -1;
                }
                fill_zero_page(page);
                long readbytes = file_->read(reinterpret_cast<uint64>(page),
                                             bytes_to_read,
                                             static_cast<long>(file_offset),
                                             false);
                if (readbytes < 0)
                {
                    mem::k_pmm.free_page(page);
                    return static_cast<int>(readbytes);
                }
                // 文件映射的页先清零再读取；普通文件被 ftruncate 扩展出的稀疏洞
                // 允许底层 read 返回短读，未覆盖的部分按文件洞语义保持为 0。

                uint64 candidate_pa = reinterpret_cast<uint64>(page);
                SpinLockGuard guard(object_lock_);
                source_pa = find_source_page(object_page_index);
                if (source_pa == 0)
                {
                    source_pages_[object_page_index] = candidate_pa;
                    source_pa = candidate_pa;
                    candidate_pa = 0;
                }
                if (candidate_pa != 0)
                {
                    mem::k_pmm.free_page(page_pa_to_kernel_ptr(candidate_pa));
                }
            }
            else
            {
                if (!zero_fill_past_file_ && !area.zero_fill_past_file)
                {
                    return signal_sigbus_for_current_task(view);
                }

                void *page = mem::PhysicalMemoryManager::try_alloc_page();
                if (page == nullptr)
                {
                    return -1;
                }
                fill_zero_page(page);
                uint64 candidate_pa = reinterpret_cast<uint64>(page);

                SpinLockGuard guard(object_lock_);
                source_pa = find_source_page(object_page_index);
                if (source_pa == 0)
                {
                    source_pages_[object_page_index] = candidate_pa;
                    source_pa = candidate_pa;
                    candidate_pa = 0;
                }
                if (candidate_pa != 0)
                {
                    mem::k_pmm.free_page(page_pa_to_kernel_ptr(candidate_pa));
                }
            }
        }

        if (area.is_private_mapping() && access_type == 1)
        {
            const void *src = page_pa_to_kernel_ptr(source_pa);
            uint64 overlay_pa = allocate_private_overlay_page(area, page_index, src, PGSIZE, false);
            if (overlay_pa == 0)
            {
                return -1;
            }
            view.pa = overlay_pa;
            view.writable = true;
            view.private_overlay = true;
            return retain_page_for_mapping(view.pa) ? 0 : -1;
        }

        view.pa = source_pa;
        view.writable = shared_mapping() && (area.prot & PROT_WRITE) != 0;
        view.mark_cow = !shared_mapping() && (area.prot & PROT_WRITE) != 0;
        return retain_page_for_mapping(view.pa) ? 0 : -1;
    }

    int FileVmObject::sync_area_range(const VmArea &area, uint64 start, uint64 end)
    {
        if (file_ == nullptr || !shared_mapping() || (area.prot & PROT_WRITE) == 0)
        {
            return 0;
        }

        if (end <= start)
        {
            return 0;
        }

        for (uint64 page_va = PGROUNDDOWN(start); page_va < PGROUNDUP(end); page_va += PGSIZE)
        {
            uint64 page_index = area.page_index_for_va(page_va);
            uint64 object_page_index = (area.page_offset / PGSIZE) + page_index;
            auto found = source_pages_.find(object_page_index);
            if (found == source_pages_.end())
            {
                continue;
            }
            long write_ret = file_->write(found->second, PGSIZE, static_cast<long>(area.mapped_page_offset(page_va)), false);
            if (write_ret < 0)
            {
                return static_cast<int>(write_ret);
            }
        }
        return 0;
    }

    SysvShmVmObject::SysvShmVmObject(const SysvShmMetadata &meta)
        : VmObject(VmObjectKind::SysvShm, true), meta_(meta)
    {
    }

    SysvShmVmObject::~SysvShmVmObject() = default;

    int SysvShmVmObject::prepare_page(VmArea &area, uint64 page_index, int, VmPageView &view)
    {
        (void)area;
        SpinLockGuard guard(object_lock_);
        uint64 source_pa = ensure_source_page(page_index, true);
        if (source_pa == 0)
        {
            return -1;
        }
        view.pa = source_pa;
        view.writable = true;
        return retain_page_for_mapping(view.pa) ? 0 : -1;
    }
}

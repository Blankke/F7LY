#include "vm_object.hh"

#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "fs/vfs/file/file.hh"
#include "fs/vfs/virtual_fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "file_page_cache.hh"
#include "libs/perf_diag.hh"
#include "printer.hh"
#include "proc/signal.hh"
#include "proc_manager.hh"
#include "scheduler.hh"
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

        constexpr uint32 k_user_page_reclaim_batch = 256;

        inline void *try_alloc_user_page(bool zero_initialize = true)
        {
            void *page = zero_initialize
                             ? mem::PhysicalMemoryManager::try_alloc_page()
                             : mem::PhysicalMemoryManager::try_alloc_page_uninitialized();
            if (page != nullptr)
            {
                return page;
            }

            file_page_cache::reclaim_clean_pages(k_user_page_reclaim_batch);
            return zero_initialize
                       ? mem::PhysicalMemoryManager::try_alloc_page()
                       : mem::PhysicalMemoryManager::try_alloc_page_uninitialized();
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

        void *page = try_alloc_user_page();
        if (page == nullptr)
        {
            return 0;
        }
        // try_alloc_user_page() 返回的普通页已经清零；参数保留对象接口语义。
        (void)zero_fill;
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
        void *page = try_alloc_user_page();
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
                               const eastl::string &cache_key,
                               uint64 initial_file_size,
                               uint64 initial_file_size_epoch)
        : VmObject(VmObjectKind::File, shared_mapping),
          file_(file),
          zero_fill_past_file_(zero_fill_past_file),
          cache_key_(cache_key)
    {
        if (file_ != nullptr)
        {
            file_->dup();
            fs::FilePageCacheIdentity identity{};
            if (file_->get_file_page_cache_identity(identity))
            {
                cache_mount_identity_ = identity.mount_identity;
                cache_inode_ = identity.inode;
                cache_inode_generation_ = identity.inode_generation;
                cache_identity_valid_ = true;
                if (initial_file_size != ~static_cast<uint64>(0) &&
                    initial_file_size_epoch != 0 &&
                    file_page_cache::content_epoch(identity) == initial_file_size_epoch)
                {
                    cached_file_size_ = initial_file_size;
                    cached_file_size_epoch_ = initial_file_size_epoch;
                    cached_file_size_valid_ = true;
                }
            }
        }
    }

    FileVmObject::~FileVmObject()
    {
        // retired 页仍持有 FileVmObject/source owner 引用；已安装的 PTE 和
        // 全局 cache（若尚未驱逐）各有独立引用，因此这里只归还本对象份额。
        for (uint64 page : retired_source_pages_)
        {
            if (page != 0)
            {
                mem::k_pmm.free_page(reinterpret_cast<void *>(page));
            }
        }
        retired_source_pages_.clear();
        source_page_epochs_.clear();
        if (file_ != nullptr)
        {
            file_->free_file();
            file_ = nullptr;
        }
    }

    bool FileVmObject::matches_cache_identity(
        const fs::FilePageCacheIdentity &identity) const
    {
        return cache_identity_valid_ &&
               cache_mount_identity_ == identity.mount_identity &&
               cache_inode_ == identity.inode &&
               cache_inode_generation_ == identity.inode_generation;
    }

    void FileVmObject::retire_source_pages_from(uint64 first_file_page)
    {
        SpinLockGuard guard(object_lock_);
        for (auto it = source_pages_.begin(); it != source_pages_.end();)
        {
            auto current = it++;
            if (current->first < first_file_page)
            {
                continue;
            }
            if (current->second != 0)
            {
                // 并发 fault/COW 可能已在 object_lock_ 外拿到该 PA。
                // 先移入 retired 保留 owner，对象析构时再统一归还。
                retired_source_pages_.push_back(current->second);
            }
            source_page_epochs_.erase(current->first);
            source_pages_.erase(current);
        }
        cached_file_size_valid_ = false;
    }

    int FileVmObject::prepare_page(VmArea &area,
                                   uint64 page_index,
                                   int access_type,
                                   VmPageView &view)
    {
        F7LY_PERF_ADD(FileFault, 1);
        // 已私有化页不随普通 file write 替换。truncate shrink
        // 则在 syscall 返回前持 mm lock 撤销越 EOF overlay/PTE。
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

        if (!cache_identity_valid_)
        {
            return prepare_page_for_sequence(area, page_index, access_type, 0, view);
        }

        constexpr int k_max_content_restarts = 16;
        constexpr int k_retry_content = -4095;
        const fs::FilePageCacheIdentity identity{
            cache_mount_identity_, cache_inode_, cache_inode_generation_};
        for (int attempt = 0; attempt < k_max_content_restarts; ++attempt)
        {
            file_page_cache::ContentState before{};
            do
            {
                before = file_page_cache::content_state(identity);
                if (before.active_mutations != 0)
                {
                    // 不持任何 spinlock；truncate 只在 end_mutation 之后才
                    // 取 mm lock 做 zap，因此此处持递归 memory_lock 让出不成环。
                    k_scheduler.yield();
                }
            } while (before.active_mutations != 0);
            if (before.sequence == 0)
            {
                return -1;
            }

            VmPageView candidate{};
            const int result = prepare_page_for_sequence(area,
                                                         page_index,
                                                         access_type,
                                                         before.sequence,
                                                         candidate);
            if (result == k_retry_content)
            {
                continue;
            }
            if (result != 0)
            {
                return result;
            }
            if (candidate.signal_delivered)
            {
                view = candidate;
                return 0;
            }

            if (file_page_cache::content_state_matches(identity, before.sequence))
            {
                view = candidate;
                return 0;
            }

            // helper 返回的 pa 已为 PTE 预留一份 mapping ref。
            // sequence 变化时未安装 PTE，先归还该 ref。
            if (candidate.pa != 0)
            {
                mem::k_pmm.free_page(page_pa_to_kernel_ptr(candidate.pa));
            }
            if (candidate.private_overlay && area.private_page_overlay != nullptr)
            {
                auto created = area.private_page_overlay->find(page_index);
                if (created != area.private_page_overlay->end() &&
                    created->second == candidate.pa)
                {
                    // 这次竞态 fault 新建的 overlay owner 也不能留给
                    // 下一轮当成稳定私有页。
                    if (created->second != 0)
                    {
                        mem::k_pmm.free_page(page_pa_to_kernel_ptr(created->second));
                    }
                    area.private_page_overlay->erase(created);
                    if (area.private_page_overlay->empty())
                    {
                        delete area.private_page_overlay;
                        area.private_page_overlay = nullptr;
                    }
                }
            }
        }
        // 连续 writer 下有界重启，不会将无法复核的页暴露给用户。
        return -1;
    }

    int FileVmObject::prepare_page_for_sequence(VmArea &area,
                                                uint64 page_index,
                                                int access_type,
                                                uint64 expected_sequence,
                                                VmPageView &view)
    {
        constexpr int k_retry_content = -4095;

        uint64 object_page_index = (area.page_offset / PGSIZE) + page_index;
        uint64 file_offset = area.page_offset + page_index * PGSIZE;
        uint64 page_offset_in_area = page_index * PGSIZE;
        const fs::FilePageCacheIdentity page_cache_identity{
            cache_mount_identity_, cache_inode_, cache_inode_generation_};
        const uint64 observed_content_epoch = expected_sequence;
        uint64 current_file_size = 0;
        bool have_current_file_size = false;

        // mmap 初始 epoch 直接使用建图时 fstat 的快照；只有 write/truncate
        // 推进内容 epoch 后，才在 fault 慢路径重新 fstat 一次。I/O 绝不持
        // object_lock_，并在发布尺寸前复核 epoch，避免缓存一次竞态旧尺寸。
        if (cache_identity_valid_ &&
            !zero_fill_past_file_ &&
            !area.zero_fill_past_file)
        {
            {
                SpinLockGuard guard(object_lock_);
                if (cached_file_size_valid_ &&
                    cached_file_size_epoch_ == observed_content_epoch)
                {
                    current_file_size = cached_file_size_;
                    have_current_file_size = true;
                }
            }
            if (!have_current_file_size)
            {
                fs::Kstat stat{};
                const int stat_result = fs::k_vfs.fstat(file_, &stat);
                if (stat_result != EOK)
                {
                    return stat_result < 0 ? stat_result : -stat_result;
                }
                if (!file_page_cache::content_state_matches(page_cache_identity,
                                                            observed_content_epoch))
                {
                    return k_retry_content;
                }

                current_file_size = stat.size;
                have_current_file_size = true;
                SpinLockGuard guard(object_lock_);
                cached_file_size_ = current_file_size;
                cached_file_size_epoch_ = observed_content_epoch;
                cached_file_size_valid_ = true;
            }
        }

        uint64 file_backed_bytes = area.file_backed_bytes;
        if (have_current_file_size)
        {
            const uint64 area_length = static_cast<uint64>(area.len);
            if (area.page_offset < current_file_size)
            {
                const uint64 remaining = current_file_size - area.page_offset;
                file_backed_bytes = remaining < area_length ? remaining : area_length;
            }
            else
            {
                file_backed_bytes = 0;
            }
        }
        const bool page_has_file_data = file_backed_bytes != 0 &&
                                        page_offset_in_area < file_backed_bytes;
        const bool private_clean_cache_candidate = cache_identity_valid_ &&
                                                   area.is_private_mapping();

        auto find_source_for_epoch_locked = [&](uint64 wanted_epoch) -> uint64
        {
            uint64 found_page = find_source_page(object_page_index);
            if (found_page == 0 || wanted_epoch == 0)
            {
                return found_page;
            }
            auto epoch = source_page_epochs_.find(object_page_index);
            if (epoch != source_page_epochs_.end() && epoch->second == wanted_epoch)
            {
                return found_page;
            }

            // 不立即 free：另一个 CPU 可能已在 object_lock_ 外拿到该地址，
            // 正准备 retain PTE 或复制 MAP_PRIVATE overlay。对象析构再归还 owner。
            retired_source_pages_.push_back(found_page);
            source_pages_.erase(object_page_index);
            source_page_epochs_.erase(object_page_index);
            return 0;
        };

        uint64 source_pa = 0;
        {
            SpinLockGuard guard(object_lock_);
            source_pa = find_source_for_epoch_locked(observed_content_epoch);
        }
        if (source_pa == 0)
        {
            if (file_ == nullptr)
            {
                return -1;
            }

            if (page_has_file_data)
            {
                uint64 bytes_remaining = file_backed_bytes - page_offset_in_area;
                size_t bytes_to_read = bytes_remaining > PGSIZE ? PGSIZE : static_cast<size_t>(bytes_remaining);
                const bool full_file_page = bytes_to_read == PGSIZE;
                uint64 candidate_pa = 0;
                uint64 candidate_epoch = observed_content_epoch;
                bool cache_access = false;
                bool cache_hit = false;
                bool run_readahead = false;

                // 缓存页必须永远保持只读。MAP_PRIVATE 写入由现有 COW/overlay
                // 私有化；所有 MAP_SHARED（包括当前只读、未来可能出现可写 alias）
                // 保守绕过，避免共享对象把 clean cache 页原地改脏。
                if (full_file_page && private_clean_cache_candidate)
                {
                    file_page_cache::AcquireResult cached =
                        file_page_cache::acquire_clean_page(file_,
                                                            page_cache_identity,
                                                            object_page_index,
                                                            file_offset);
                    if (cached.status == file_page_cache::AcquireStatus::Error)
                    {
                        return cached.error != 0 ? cached.error : -1;
                    }
                    if (cached.status == file_page_cache::AcquireStatus::Retry)
                    {
                        return k_retry_content;
                    }
                    if (cached.status == file_page_cache::AcquireStatus::Acquired)
                    {
                        if (cached.content_epoch != observed_content_epoch)
                        {
                            mem::k_pmm.free_page(page_pa_to_kernel_ptr(cached.page));
                            return k_retry_content;
                        }
                        candidate_pa = cached.page;
                        candidate_epoch = cached.content_epoch;
                        cache_access = true;
                        cache_hit = cached.hit;
                    }
                }

                if (candidate_pa == 0)
                {
                    void *page = try_alloc_user_page(!full_file_page);
                    if (page == nullptr)
                    {
                        return -1;
                    }
                    long readbytes = file_->read(reinterpret_cast<uint64>(page),
                                                 bytes_to_read,
                                                 static_cast<long>(file_offset),
                                                 false);
                    if (readbytes < 0)
                    {
                        mem::k_pmm.free_page(page);
                        return static_cast<int>(readbytes);
                    }
                    size_t initialized_bytes = static_cast<size_t>(readbytes);
                    if (initialized_bytes > bytes_to_read)
                    {
                        initialized_bytes = bytes_to_read;
                    }
                    F7LY_PERF_ADD(FileFaultReadBytes, initialized_bytes);
                    if (initialized_bytes < PGSIZE)
                    {
                        memset(reinterpret_cast<char *>(page) + initialized_bytes,
                               0,
                               PGSIZE - initialized_bytes);
                    }
                    candidate_pa = reinterpret_cast<uint64>(page);
                    if (cache_identity_valid_ &&
                        !file_page_cache::content_state_matches(page_cache_identity,
                                                                observed_content_epoch))
                    {
                        mem::k_pmm.free_page(page);
                        return k_retry_content;
                    }
                }

                {
                    SpinLockGuard guard(object_lock_);
                    source_pa = find_source_for_epoch_locked(candidate_epoch);
                    if (source_pa == 0)
                    {
                        source_pages_[object_page_index] = candidate_pa;
                        source_page_epochs_[object_page_index] = candidate_epoch;
                        source_pa = candidate_pa;
                        candidate_pa = 0;
                    }

                    bool trigger_readahead = false;
                    if (cache_access)
                    {
                        constexpr uint8 k_sequential_window_size = 10;
                        constexpr uint16 k_sequential_window_mask =
                            (1U << k_sequential_window_size) - 1U;
                        if (sequential_last_page_ != ~static_cast<uint64>(0) &&
                            object_page_index == sequential_last_page_ + 1)
                        {
                            // 保留当前固定窗口；下面统一左移并加入这次 hit/miss。
                        }
                        else
                        {
                            sequential_miss_window_ = 0;
                            sequential_window_count_ = 0;
                            readahead_until_page_ = object_page_index + 1;
                        }
                        sequential_miss_window_ = static_cast<uint16>(
                            ((sequential_miss_window_ << 1) | (cache_hit ? 0U : 1U)) &
                            k_sequential_window_mask);
                        if (sequential_window_count_ < k_sequential_window_size)
                        {
                            ++sequential_window_count_;
                        }
                        sequential_last_page_ = object_page_index;
                        // freestanding 链接不带 libgcc 的 __popcountdi2，固定 10 bit
                        // 小窗口直接用 SWAR 计数，避免编译器生成运行库调用。
                        uint32 miss_bits = sequential_miss_window_;
                        miss_bits = miss_bits - ((miss_bits >> 1) & 0x5555U);
                        miss_bits = (miss_bits & 0x3333U) +
                                    ((miss_bits >> 2) & 0x3333U);
                        miss_bits = (miss_bits + (miss_bits >> 4)) & 0x0f0fU;
                        miss_bits += miss_bits >> 8;
                        const uint32 window_misses = miss_bits & 0x1fU;
                        const uint64 next_area_offset = page_offset_in_area + PGSIZE;
                        const uint64 remaining = file_backed_bytes > next_area_offset
                                                     ? file_backed_bytes - next_area_offset
                                                     : 0;
                        if (sequential_window_count_ == k_sequential_window_size &&
                            window_misses >= 7 &&
                            object_page_index + 1 >= readahead_until_page_ &&
                            remaining >= 16 * PGSIZE)
                        {
                            trigger_readahead = true;
                            readahead_until_page_ = object_page_index + 1 + 16;
                        }
                    }

                    if (candidate_pa != 0)
                    {
                        mem::k_pmm.free_page(page_pa_to_kernel_ptr(candidate_pa));
                    }

                    if (trigger_readahead)
                    {
                        // guard 在作用域末才释放；这里只记录触发，实际文件
                        // I/O 严格放到 object_lock_ 之外。
                        run_readahead = true;
                    }
                }

                if (run_readahead)
                {
                    file_page_cache::readahead_16_pages(file_,
                                                        page_cache_identity,
                                                        object_page_index + 1,
                                                        file_offset + PGSIZE);
                }
            }
            else
            {
                if (!zero_fill_past_file_ && !area.zero_fill_past_file)
                {
                    if (cache_identity_valid_ &&
                        !file_page_cache::content_state_matches(page_cache_identity,
                                                                observed_content_epoch))
                    {
                        return k_retry_content;
                    }
                    return signal_sigbus_for_current_task(view);
                }

                void *page = try_alloc_user_page();
                if (page == nullptr)
                {
                    return -1;
                }
                uint64 candidate_pa = reinterpret_cast<uint64>(page);

                SpinLockGuard guard(object_lock_);
                source_pa = find_source_for_epoch_locked(observed_content_epoch);
                if (source_pa == 0)
                {
                    source_pages_[object_page_index] = candidate_pa;
                    source_page_epochs_[object_page_index] = observed_content_epoch;
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
            uint64 source_pa = 0;
            {
                SpinLockGuard guard(object_lock_);
                source_pa = find_source_page(object_page_index);
            }
            if (source_pa == 0)
            {
                continue;
            }
            // source_pages_ 只在对象析构时删除；当前 area 引用保证解锁后
            // 这张页仍然有效，因此不需要持有自旋锁进入可睡眠的文件写路径。
            long write_ret = file_->write(source_pa,
                                           PGSIZE,
                                           static_cast<long>(area.mapped_page_offset(page_va)),
                                           false);
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

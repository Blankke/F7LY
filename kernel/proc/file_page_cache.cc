#include "file_page_cache.hh"

#include "devs/spinlock.hh"
#include "fs/vfs/file/file.hh"
#include "libs/perf_diag.hh"
#include "libs/string.hh"
#include "mem/heap_memory_manager.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/page.hh"

#include <EASTL/atomic.h>
#include <EASTL/unordered_map.h>

namespace proc::file_page_cache
{
    namespace
    {
        constexpr uint32 k_shard_count = 64;
        constexpr uint64 k_full_invalidation = ~static_cast<uint64>(0);
        constexpr uint64 k_min_capacity_bytes = 128ULL * 1024 * 1024;
        constexpr uint64 k_max_capacity_bytes = 1024ULL * 1024 * 1024;
        constexpr uint32 k_large_invalidation_pages = 4096;
        constexpr uint32 k_low_water_reclaim_batch = 256;
        constexpr uint32 k_readahead_pages = 16;

        uint64 mix64(uint64 value)
        {
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }

        uint64 identity_hash(const fs::FilePageCacheIdentity &identity)
        {
            uint64 hash = mix64(identity.mount_identity);
            hash ^= mix64((static_cast<uint64>(identity.inode) << 32) |
                          identity.inode_generation);
            return mix64(hash);
        }

        struct IdentityHash
        {
            size_t operator()(const fs::FilePageCacheIdentity &identity) const
            {
                return static_cast<size_t>(identity_hash(identity));
            }
        };

        struct PageKey
        {
            fs::FilePageCacheIdentity identity{};
            uint64 page_index = 0;

            bool operator==(const PageKey &other) const
            {
                return page_index == other.page_index && identity == other.identity;
            }
        };

        struct PageKeyHash
        {
            size_t operator()(const PageKey &key) const
            {
                return static_cast<size_t>(mix64(identity_hash(key.identity) ^
                                                 mix64(key.page_index)));
            }
        };

        struct CacheEntry
        {
            PageKey key{};
            uint64 page = 0;
            uint64 content_epoch = 0;
            CacheEntry *lru_prev = nullptr;
            CacheEntry *lru_next = nullptr;
            CacheEntry *garbage_next = nullptr;
        };

        struct PageShard
        {
            SpinLock lock;
            eastl::unordered_map<PageKey, CacheEntry *, PageKeyHash> pages;
            CacheEntry *lru_head = nullptr;
            CacheEntry *lru_tail = nullptr;
        };

        struct EpochState
        {
            uint64 sequence = 1;
            uint32 active_mutations = 0;
        };

        struct EpochShard
        {
            SpinLock lock;
            eastl::unordered_map<fs::FilePageCacheIdentity, EpochState, IdentityHash> epochs;
        };

        struct CacheState
        {
            CacheState()
            {
                budget_lock.init("file_page_budget");
                for (uint32 index = 0; index < k_shard_count; ++index)
                {
                    page_shards[index].lock.init("file_page_shard");
                    epoch_shards[index].lock.init("file_epoch_shard");
                }
            }

            SpinLock budget_lock;
            eastl::atomic<bool> budget_ready{false};
            uint64 capacity_pages = 0;
            uint64 per_shard_capacity = 0;
            uint64 low_water_pages = 0;
            uint64 high_water_pages = 0;
            PageShard page_shards[k_shard_count];
            EpochShard epoch_shards[k_shard_count];
        };

        // freestanding 内核不会执行编译器生成的 .init_array，不能把包含
        // EASTL 容器的 CacheState 直接定义成全局对象。由 boot main 在
        // HMM 就绪后显式构造，避免在零初始化但未构造的哈希表上操作。
        CacheState *g_cache = nullptr;

        inline CacheState &cache_state()
        {
            if (g_cache == nullptr)
            {
                panic("file_page_cache used before init");
            }
            return *g_cache;
        }

        bool identity_valid(const fs::FilePageCacheIdentity &identity)
        {
            return identity.mount_identity != 0 && identity.inode != 0;
        }

        uint32 epoch_shard_index(const fs::FilePageCacheIdentity &identity)
        {
            return static_cast<uint32>(identity_hash(identity) & (k_shard_count - 1));
        }

        uint32 page_shard_index(const PageKey &key)
        {
            return static_cast<uint32>(PageKeyHash{}(key) & (k_shard_count - 1));
        }

        EpochState &epoch_slot_locked(EpochShard &shard,
                                      const fs::FilePageCacheIdentity &identity)
        {
            EpochState &state = shard.epochs[identity];
            if (state.sequence == 0)
            {
                state.sequence = 1;
            }
            return state;
        }

        void advance_sequence_locked(EpochState &state)
        {
            ++state.sequence;
            if (state.sequence == 0)
            {
                state.sequence = 1;
            }
        }

        void ensure_budget()
        {
            if (cache_state().budget_ready.load(eastl::memory_order_acquire))
            {
                return;
            }
            cache_state().budget_lock.acquire();
            if (!cache_state().budget_ready.load(eastl::memory_order_relaxed))
            {
                const uint64 managed_pages = mem::k_pmm.get_page_count();
                const uint64 free_pages = mem::k_pmm.get_free_page_count();
                const uint64 min_pages = k_min_capacity_bytes / PGSIZE;
                const uint64 max_pages = k_max_capacity_bytes / PGSIZE;

                uint64 desired_pages = managed_pages / 8;
                if (desired_pages < min_pages)
                {
                    desired_pages = min_pages;
                }
                if (desired_pages > max_pages)
                {
                    desired_pages = max_pages;
                }

                // min 128 MiB 是正常机器的目标下限；低内存启动时仍必须给匿名页、
                // 页表和内核堆留出余量，不能为性能缓存耗尽实际空闲页。
                const uint64 reserve_pages = managed_pages / 32 > 4096
                                                  ? managed_pages / 32
                                                  : 4096;
                const uint64 available_budget = free_pages > reserve_pages
                                                    ? free_pages - reserve_pages
                                                    : 0;
                cache_state().capacity_pages = desired_pages < available_budget
                                             ? desired_pages
                                             : available_budget;
                cache_state().per_shard_capacity =
                    (cache_state().capacity_pages + k_shard_count - 1) / k_shard_count;
                cache_state().low_water_pages = reserve_pages;
                const uint64 hysteresis = managed_pages / 64 > 1024
                                              ? managed_pages / 64
                                              : 1024;
                cache_state().high_water_pages = reserve_pages + hysteresis;
                cache_state().budget_ready.store(true, eastl::memory_order_release);
            }
            cache_state().budget_lock.release();
        }

        void lru_remove_locked(PageShard &shard, CacheEntry *entry)
        {
            if (entry->lru_prev != nullptr)
            {
                entry->lru_prev->lru_next = entry->lru_next;
            }
            else
            {
                shard.lru_head = entry->lru_next;
            }
            if (entry->lru_next != nullptr)
            {
                entry->lru_next->lru_prev = entry->lru_prev;
            }
            else
            {
                shard.lru_tail = entry->lru_prev;
            }
            entry->lru_prev = nullptr;
            entry->lru_next = nullptr;
        }

        void lru_insert_head_locked(PageShard &shard, CacheEntry *entry)
        {
            entry->lru_prev = nullptr;
            entry->lru_next = shard.lru_head;
            if (shard.lru_head != nullptr)
            {
                shard.lru_head->lru_prev = entry;
            }
            else
            {
                shard.lru_tail = entry;
            }
            shard.lru_head = entry;
        }

        void lru_touch_locked(PageShard &shard, CacheEntry *entry)
        {
            if (shard.lru_head == entry)
            {
                return;
            }
            lru_remove_locked(shard, entry);
            lru_insert_head_locked(shard, entry);
        }

        void add_garbage(CacheEntry *&garbage, CacheEntry *entry)
        {
            entry->garbage_next = garbage;
            garbage = entry;
        }

        void detach_entry_locked(PageShard &shard,
                                 CacheEntry *entry,
                                 CacheEntry *&garbage)
        {
            lru_remove_locked(shard, entry);
            shard.pages.erase(entry->key);
            add_garbage(garbage, entry);
        }

        void release_garbage(CacheEntry *garbage, bool count_evictions)
        {
            while (garbage != nullptr)
            {
                CacheEntry *next = garbage->garbage_next;
                if (garbage->page != 0)
                {
                    mem::k_pmm.free_page(reinterpret_cast<void *>(garbage->page));
                }
                delete garbage;
                if (count_evictions)
                {
                    F7LY_PERF_ADD(FileCacheEvict, 1);
                }
                garbage = next;
            }
        }

        void trim_shard_locked(PageShard &shard, CacheEntry *&garbage)
        {
            while (cache_state().per_shard_capacity != 0 &&
                   shard.pages.size() > cache_state().per_shard_capacity &&
                   shard.lru_tail != nullptr)
            {
                detach_entry_locked(shard, shard.lru_tail, garbage);
            }
        }

        void maybe_reclaim_low_water(uint64 sampled_free_pages)
        {
            if (sampled_free_pages > cache_state().low_water_pages)
            {
                return;
            }

            CacheEntry *garbage = nullptr;
            uint32 detached = 0;
            for (uint32 index = 0;
                 index < k_shard_count && detached < k_low_water_reclaim_batch;
                 ++index)
            {
                PageShard &shard = cache_state().page_shards[index];
                shard.lock.acquire();
                while (shard.lru_tail != nullptr &&
                       detached < k_low_water_reclaim_batch)
                {
                    detach_entry_locked(shard, shard.lru_tail, garbage);
                    ++detached;
                    if ((detached & 3U) == 0)
                    {
                        break;
                    }
                }
                shard.lock.release();
            }
            release_garbage(garbage, true);
        }

        bool range_contains_page(uint64 start_page,
                                 uint64 page_count,
                                 uint64 page_index)
        {
            return page_index >= start_page &&
                   page_index - start_page < page_count;
        }

        CacheEntry *find_entry_locked(PageShard &shard, const PageKey &key)
        {
            auto found = shard.pages.find(key);
            return found == shard.pages.end() ? nullptr : found->second;
        }

        struct InvalidationRange
        {
            bool all = false;
            uint64 start_page = 0;
            uint64 page_count = 0;
        };

        InvalidationRange make_invalidation_range(uint64 offset, uint64 length)
        {
            InvalidationRange range{};
            range.all = length == k_full_invalidation ||
                        offset > k_full_invalidation - length;
            range.start_page = offset / PGSIZE;
            if (range.all)
            {
                return range;
            }

            const uint64 end = offset + length;
            const uint64 rounded_end = end > k_full_invalidation - (PGSIZE - 1)
                                                   ? k_full_invalidation
                                                   : end + (PGSIZE - 1);
            const uint64 end_page = rounded_end / PGSIZE;
            range.page_count = end_page > range.start_page
                                   ? end_page - range.start_page
                                   : 1;
            return range;
        }

        // 调用时必须持有该 identity 的 epoch shard lock。页缓存锁
        // 始终在 epoch 锁之后获取，使 begin/end 与 acquire 发布线性化。
        void detach_range_locked(const fs::FilePageCacheIdentity &identity,
                                 const InvalidationRange &range,
                                 CacheEntry *&garbage)
        {
            if (range.all || range.page_count > k_large_invalidation_pages)
            {
                for (uint32 index = 0; index < k_shard_count; ++index)
                {
                    PageShard &shard = cache_state().page_shards[index];
                    shard.lock.acquire();
                    for (auto it = shard.pages.begin(); it != shard.pages.end();)
                    {
                        auto current = it++;
                        CacheEntry *entry = current->second;
                        if (!(entry->key.identity == identity) ||
                            (!range.all &&
                             !range_contains_page(range.start_page,
                                                  range.page_count,
                                                  entry->key.page_index)))
                        {
                            continue;
                        }
                        lru_remove_locked(shard, entry);
                        shard.pages.erase(current);
                        add_garbage(garbage, entry);
                    }
                    shard.lock.release();
                }
                return;
            }

            for (uint64 index = 0; index < range.page_count; ++index)
            {
                const PageKey key{identity, range.start_page + index};
                PageShard &shard = cache_state().page_shards[page_shard_index(key)];
                shard.lock.acquire();
                CacheEntry *entry = find_entry_locked(shard, key);
                if (entry != nullptr)
                {
                    detach_entry_locked(shard, entry, garbage);
                }
                shard.lock.release();
            }
        }

        bool retain_cache_page(uint64 page)
        {
            return page != 0 &&
                   mem::k_pmm.retain_page(reinterpret_cast<void *>(page));
        }
    }

    void init()
    {
        if (g_cache != nullptr)
        {
            panic("file_page_cache initialized twice");
        }
        g_cache = new CacheState();
        if (g_cache == nullptr)
        {
            panic("file_page_cache initialization failed");
        }
    }

    uint64 content_epoch(const fs::FilePageCacheIdentity &identity)
    {
        return content_state(identity).sequence;
    }

    ContentState content_state(const fs::FilePageCacheIdentity &identity)
    {
        ContentState result{};
        if (!identity_valid(identity))
        {
            return result;
        }
        EpochShard &shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        shard.lock.acquire();
        const EpochState &state = epoch_slot_locked(shard, identity);
        result.sequence = state.sequence;
        result.active_mutations = state.active_mutations;
        shard.lock.release();
        return result;
    }

    bool content_state_matches(const fs::FilePageCacheIdentity &identity,
                               uint64 sequence)
    {
        if (!identity_valid(identity) || sequence == 0)
        {
            return false;
        }
        EpochShard &shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        shard.lock.acquire();
        const EpochState &state = epoch_slot_locked(shard, identity);
        const bool matches = state.active_mutations == 0 &&
                             state.sequence == sequence;
        shard.lock.release();
        return matches;
    }

    AcquireResult acquire_clean_page(fs::file *file,
                                     const fs::FilePageCacheIdentity &identity,
                                     uint64 page_index,
                                     uint64 file_offset)
    {
        AcquireResult result{};
        if (file == nullptr || !identity_valid(identity))
        {
            return result;
        }

        ensure_budget();
        if (cache_state().capacity_pages == 0 || cache_state().per_shard_capacity == 0)
        {
            return result;
        }

        const PageKey key{identity, page_index};
        EpochShard &epoch_shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        PageShard &page_shard = cache_state().page_shards[page_shard_index(key)];

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            epoch_shard.lock.acquire();
            const EpochState &observed_state = epoch_slot_locked(epoch_shard, identity);
            if (observed_state.active_mutations != 0)
            {
                epoch_shard.lock.release();
                result.status = AcquireStatus::Retry;
                return result;
            }
            const uint64 observed_epoch = observed_state.sequence;
            page_shard.lock.acquire();
            CacheEntry *existing = find_entry_locked(page_shard, key);
            if (existing != nullptr)
            {
                existing->content_epoch = observed_epoch;
                lru_touch_locked(page_shard, existing);
                const bool retained = retain_cache_page(existing->page);
                result.page = retained ? existing->page : 0;
                result.content_epoch = observed_epoch;
                result.status = retained ? AcquireStatus::Acquired : AcquireStatus::Error;
                result.error = retained ? 0 : -1;
                result.hit = retained;
                page_shard.lock.release();
                epoch_shard.lock.release();
                if (retained)
                {
                    F7LY_PERF_ADD(FileCacheHit, 1);
                }
                return result;
            }
            page_shard.lock.release();
            epoch_shard.lock.release();

            F7LY_PERF_ADD(FileCacheMiss, 1);
            // 命中路径只需 epoch/shard 和 PMM retain；空闲页全局计数、
            // 低水位回收与可睡眠的 clean 检查均只放在真正 miss 后。
            const uint64 sampled_free_pages = mem::k_pmm.get_free_page_count();
            maybe_reclaim_low_water(sampled_free_pages);
            if (sampled_free_pages <= cache_state().low_water_pages)
            {
                return result;
            }

            // 确认没有尚未提交的 write-combine 数据。此调用可能睡眠，
            // 必须位于所有 cache 自旋锁之外。
            if (!file->file_page_cache_is_clean())
            {
                return result;
            }

            void *candidate_page = mem::k_pmm.try_alloc_page_uninitialized();
            if (candidate_page == nullptr)
            {
                result.status = AcquireStatus::Error;
                result.error = -1;
                return result;
            }

            const long read_bytes = file->read(reinterpret_cast<uint64>(candidate_page),
                                               PGSIZE,
                                               static_cast<long>(file_offset),
                                               false);
            if (read_bytes < 0)
            {
                mem::k_pmm.free_page(candidate_page);
                result.status = AcquireStatus::Error;
                result.error = static_cast<int>(read_bytes);
                return result;
            }
            size_t initialized = static_cast<size_t>(read_bytes);
            if (initialized > PGSIZE)
            {
                initialized = PGSIZE;
            }
            F7LY_PERF_ADD(FileFaultReadBytes, initialized);
            if (initialized < PGSIZE)
            {
                memset(reinterpret_cast<uint8 *>(candidate_page) + initialized,
                       0,
                       PGSIZE - initialized);
            }

            CacheEntry *candidate_entry = new CacheEntry();
            if (candidate_entry == nullptr)
            {
                mem::k_pmm.free_page(candidate_page);
                result.status = AcquireStatus::Error;
                result.error = -1;
                return result;
            }
            candidate_entry->key = key;
            candidate_entry->page = reinterpret_cast<uint64>(candidate_page);
            candidate_entry->content_epoch = observed_epoch;

            CacheEntry *garbage = nullptr;
            epoch_shard.lock.acquire();
            const EpochState &current_state = epoch_slot_locked(epoch_shard, identity);
            const uint64 current_epoch = current_state.sequence;
            if (current_state.active_mutations != 0 ||
                current_epoch != observed_epoch)
            {
                epoch_shard.lock.release();
                delete candidate_entry;
                mem::k_pmm.free_page(candidate_page);
                continue;
            }

            page_shard.lock.acquire();
            existing = find_entry_locked(page_shard, key);
            if (existing != nullptr)
            {
                existing->content_epoch = current_epoch;
                lru_touch_locked(page_shard, existing);
                const bool retained = retain_cache_page(existing->page);
                result.page = retained ? existing->page : 0;
                result.content_epoch = current_epoch;
                result.status = retained ? AcquireStatus::Acquired : AcquireStatus::Error;
                result.error = retained ? 0 : -1;
                result.hit = false;
                page_shard.lock.release();
                epoch_shard.lock.release();
                delete candidate_entry;
                mem::k_pmm.free_page(candidate_page);
                return result;
            }

            // candidate_page 的初始 owner 引用转交给 cache；再 retain 一份交给
            // FileVmObject/source。后续 PTE 安装还会独立 retain。
            if (!retain_cache_page(candidate_entry->page))
            {
                page_shard.lock.release();
                epoch_shard.lock.release();
                delete candidate_entry;
                mem::k_pmm.free_page(candidate_page);
                result.status = AcquireStatus::Error;
                result.error = -1;
                return result;
            }
            page_shard.pages[key] = candidate_entry;
            lru_insert_head_locked(page_shard, candidate_entry);
            trim_shard_locked(page_shard, garbage);
            result.status = AcquireStatus::Acquired;
            result.page = candidate_entry->page;
            result.content_epoch = current_epoch;
            result.hit = false;
            page_shard.lock.release();
            epoch_shard.lock.release();
            release_garbage(garbage, true);
            return result;
        }

        // 内容持续变化时不能回退到无门禁的私有读；
        // 交由 FileVmObject 从 size/sequence 快照整体重启。
        result.status = AcquireStatus::Retry;
        return result;
    }

    bool begin_mutation(const fs::FilePageCacheIdentity &identity,
                        uint64 offset,
                        uint64 length)
    {
        if (!identity_valid(identity) || length == 0)
        {
            return false;
        }

        const InvalidationRange range = make_invalidation_range(offset, length);
        EpochShard &epoch_shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        CacheEntry *garbage = nullptr;
        epoch_shard.lock.acquire();
        EpochState &state = epoch_slot_locked(epoch_shard, identity);
        // sequence 在 active++ 的同一个 epoch shard 临界区内推进。
        // acquire 不可能在看到新 sequence 的同时漏掉 active writer。
        advance_sequence_locked(state);
        ++state.active_mutations;
        if (state.active_mutations == 0)
        {
            panic("file_page_cache: active_mutations overflow");
        }
        detach_range_locked(identity, range, garbage);
        epoch_shard.lock.release();
        release_garbage(garbage, false);
        return true;
    }

    void end_mutation(const fs::FilePageCacheIdentity &identity,
                      uint64 offset,
                      uint64 length)
    {
        if (!identity_valid(identity) || length == 0)
        {
            return;
        }

        const InvalidationRange range = make_invalidation_range(offset, length);
        EpochShard &epoch_shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        CacheEntry *garbage = nullptr;
        epoch_shard.lock.acquire();
        EpochState &state = epoch_slot_locked(epoch_shard, identity);
        if (state.active_mutations == 0)
        {
            epoch_shard.lock.release();
            panic("file_page_cache: unmatched end_mutation");
        }
        // post detach + sequence bump 先于 active--，保证最后一个 writer
        // 打开门禁时，所有与 I/O 并发的 candidate 都已失效。
        detach_range_locked(identity, range, garbage);
        advance_sequence_locked(state);
        --state.active_mutations;
        epoch_shard.lock.release();
        release_garbage(garbage, false);
    }

    void invalidate_range(const fs::FilePageCacheIdentity &identity,
                          uint64 offset,
                          uint64 length)
    {
        if (!identity_valid(identity) || length == 0)
        {
            return;
        }

        const InvalidationRange range = make_invalidation_range(offset, length);
        EpochShard &epoch_shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        CacheEntry *garbage = nullptr;
        epoch_shard.lock.acquire();
        auto epoch_found = epoch_shard.epochs.find(identity);
        if (epoch_found == epoch_shard.epochs.end())
        {
            epoch_shard.lock.release();
            return;
        }
        detach_range_locked(identity, range, garbage);
        advance_sequence_locked(epoch_found->second);
        epoch_shard.lock.release();
        // 已安装 PTE 和 FileVmObject/source 各有自己的引用；这里仅归还 cache owner。
        release_garbage(garbage, false);
    }

    void readahead_16_pages(fs::file *file,
                            const fs::FilePageCacheIdentity &identity,
                            uint64 first_page_index,
                            uint64 first_file_offset)
    {
        if (file == nullptr || !identity_valid(identity))
        {
            return;
        }
        ensure_budget();
        if (cache_state().capacity_pages == 0)
        {
            return;
        }

        EpochShard &epoch_shard = cache_state().epoch_shards[epoch_shard_index(identity)];
        epoch_shard.lock.acquire();
        const EpochState &observed_state = epoch_slot_locked(epoch_shard, identity);
        if (observed_state.active_mutations != 0)
        {
            epoch_shard.lock.release();
            return;
        }
        const uint64 observed_epoch = observed_state.sequence;
        bool any_missing = false;
        for (uint32 index = 0; index < k_readahead_pages; ++index)
        {
            const PageKey key{identity, first_page_index + index};
            PageShard &shard = cache_state().page_shards[page_shard_index(key)];
            shard.lock.acquire();
            any_missing = any_missing || find_entry_locked(shard, key) == nullptr;
            shard.lock.release();
        }
        epoch_shard.lock.release();
        if (!any_missing)
        {
            return;
        }

        const uint64 sampled_free_pages = mem::k_pmm.get_free_page_count();
        maybe_reclaim_low_water(sampled_free_pages);
        if (sampled_free_pages <= cache_state().low_water_pages ||
            !file->file_page_cache_is_clean())
        {
            return;
        }

        constexpr size_t read_size = k_readahead_pages * PGSIZE;
        uint8 *buffer = reinterpret_cast<uint8 *>(mem::k_hmm.try_allocate(read_size));
        if (buffer == nullptr)
        {
            return;
        }

        // 唯一的 64 KiB I/O 在所有 cache 自旋锁之外完成。
        const long read_bytes = file->read(reinterpret_cast<uint64>(buffer),
                                           read_size,
                                           static_cast<long>(first_file_offset),
                                           false);
        if (read_bytes < 0)
        {
            mem::k_hmm.free(buffer);
            return;
        }
        size_t initialized_total = static_cast<size_t>(read_bytes);
        if (initialized_total > read_size)
        {
            initialized_total = read_size;
        }
        F7LY_PERF_ADD(FileFaultReadBytes, initialized_total);

        void *candidates[k_readahead_pages] = {};
        for (uint32 index = 0; index < k_readahead_pages; ++index)
        {
            void *page = mem::k_pmm.try_alloc_page_uninitialized();
            if (page == nullptr)
            {
                continue;
            }
            const size_t page_begin = static_cast<size_t>(index) * PGSIZE;
            const size_t available = initialized_total > page_begin
                                         ? ((initialized_total - page_begin) > PGSIZE
                                                ? PGSIZE
                                                : initialized_total - page_begin)
                                         : 0;
            if (available != 0)
            {
                memmove(page, buffer + page_begin, available);
            }
            if (available < PGSIZE)
            {
                memset(reinterpret_cast<uint8 *>(page) + available,
                       0,
                       PGSIZE - available);
            }
            candidates[index] = page;
        }
        mem::k_hmm.free(buffer);

        CacheEntry *garbage = nullptr;
        uint32 inserted = 0;
        epoch_shard.lock.acquire();
        const EpochState &current_state = epoch_slot_locked(epoch_shard, identity);
        const uint64 current_epoch = current_state.sequence;
        if (current_state.active_mutations == 0 &&
            current_epoch == observed_epoch)
        {
            for (uint32 index = 0; index < k_readahead_pages; ++index)
            {
                if (candidates[index] == nullptr)
                {
                    continue;
                }
                const PageKey key{identity, first_page_index + index};
                PageShard &shard = cache_state().page_shards[page_shard_index(key)];
                shard.lock.acquire();
                if (find_entry_locked(shard, key) == nullptr)
                {
                    CacheEntry *entry = new CacheEntry();
                    if (entry != nullptr)
                    {
                        entry->key = key;
                        entry->page = reinterpret_cast<uint64>(candidates[index]);
                        entry->content_epoch = current_epoch;
                        shard.pages[key] = entry;
                        lru_insert_head_locked(shard, entry);
                        trim_shard_locked(shard, garbage);
                        candidates[index] = nullptr; // 初始 owner 转交给 cache
                        ++inserted;
                    }
                }
                shard.lock.release();
            }
        }
        epoch_shard.lock.release();

        for (void *candidate : candidates)
        {
            if (candidate != nullptr)
            {
                mem::k_pmm.free_page(candidate);
            }
        }
        release_garbage(garbage, true);
        if (inserted != 0)
        {
            F7LY_PERF_ADD(FileCacheReadaheadPages, inserted);
        }
    }
}

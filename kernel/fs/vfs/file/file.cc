#include "fs/vfs/file/file.hh"

#include "proc.hh"
#include "proc_manager.hh"

#include "klib.hh"
#include "types.hh"
	#include "syscall_defs.hh"

	#include <limits.h>
	#include <EASTL/string.h>
	#include <EASTL/vector.h>

namespace fs
{
    int file::readlink( uint64 buf, size_t size )
    {
        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();

        [[maybe_unused]]dentry * cwd_ = cur_proc->_cwd;
        eastl::string abs_path = cur_proc->_name;
        eastl::string temp;
        panic("file::readlink: not implemented yet");
        // while( cwd_ )
        // {
        //     temp = cwd_->getParent()->rName() + "/";
        //     abs_path = temp + cwd_->rName();
        //     cwd_ = cwd_->getParent();
        // }

        int ret;
        size < abs_path.length() ? ret = size : ret = abs_path.length();

        memcpy( (void *)buf, abs_path.c_str(), ret);
        return ret;
    }

    eastl::string file::read_symlink_target()
    {
        panic("虚类占位实现");
        // 默认实现：返回文件路径
        return _path_name;
    }

    file_pool k_file_table;

	    namespace
	    {
	        constexpr int kMaxBsdFlockEntries = 128;
	        constexpr int kMaxBsdFlockSharedOwners = 16;
	        constexpr int kMaxPosixRecordLocks = 256;
	        constexpr int kMaxPosixRecordLockWaiters = 256;
	        constexpr int kMaxOfdRecordLocks = 256;
	        constexpr off_t kLockOpenEnd = LONG_MAX;

	        const char *record_lock_type_name(short type)
	        {
	            switch (type)
	            {
	            case F_RDLCK:
	                return "F_RDLCK";
	            case F_WRLCK:
	                return "F_WRLCK";
	            case F_UNLCK:
	                return "F_UNLCK";
	            default:
	                return "F_BAD";
	            }
	        }

	        void print_record_lock_range(const char *prefix, const struct flock &lock)
	        {
	            printf("[record-lock-debug] %s type=%s(%d) whence=%d start=%ld len=%ld pid_field=%d\n",
	                   prefix, record_lock_type_name(lock.l_type), lock.l_type,
	                   lock.l_whence, static_cast<long>(lock.l_start),
	                   static_cast<long>(lock.l_len), lock.l_pid);
	        }

        struct BsdFlockEntry
        {
            bool used = false;
            eastl::string path;
            file *exclusive_owner = nullptr;
            file *shared_owners[kMaxBsdFlockSharedOwners] = {};
            int shared_count = 0;
        };

	        struct PosixRecordLockEntry
	        {
	            bool used = false;
	            eastl::string path;
	            int pid = 0;
	            struct flock lock = {};
	        };

	        struct PosixRecordLockWaitEntry
	        {
	            bool used = false;
	            eastl::string path;
	            int waiter_pid = 0;
	            int blocked_by_pid = 0;
	            struct flock lock = {};
	        };

	        struct OfdRecordLockEntry
	        {
	            bool used = false;
	            eastl::string path;
	            file *owner = nullptr;
	            struct flock lock = {};
	        };

	        SpinLock g_bsd_flock_lock;
	        BsdFlockEntry g_bsd_flock_entries[kMaxBsdFlockEntries];
	        SpinLock g_posix_record_lock;
	        PosixRecordLockEntry g_posix_record_locks[kMaxPosixRecordLocks];
	        PosixRecordLockWaitEntry g_posix_record_lock_waiters[kMaxPosixRecordLockWaiters];
	        OfdRecordLockEntry g_ofd_record_locks[kMaxOfdRecordLocks];
	        bool g_bsd_flock_ready = false;

	        off_t record_lock_end(const struct flock &lock);

	        void dump_posix_record_locks_locked(const eastl::string &path, const char *reason)
	        {
	            int count = 0;
	            printf("[record-lock-debug] table-dump reason=%s path=%s\n",
	                   reason, path.c_str());
	            for (const auto &entry : g_posix_record_locks)
	            {
	                if (!entry.used || entry.path != path)
	                {
	                    continue;
	                }
	                printf("[record-lock-debug] table-entry idx=%d pid=%d type=%s(%d) start=%ld len=%ld end=%ld\n",
	                       count, entry.pid, record_lock_type_name(entry.lock.l_type), entry.lock.l_type,
	                       static_cast<long>(entry.lock.l_start), static_cast<long>(entry.lock.l_len),
	                       static_cast<long>(record_lock_end(entry.lock)));
	                ++count;
	            }
	            printf("[record-lock-debug] table-dump-end reason=%s count=%d\n", reason, count);
	        }

        int find_shared_owner(const BsdFlockEntry &entry, file *owner)
        {
            for (int i = 0; i < entry.shared_count; ++i)
            {
                if (entry.shared_owners[i] == owner)
                    return i;
            }
            return -1;
        }

        void remove_shared_owner(BsdFlockEntry &entry, file *owner)
        {
            int index = find_shared_owner(entry, owner);
            if (index < 0)
                return;

            for (int i = index; i + 1 < entry.shared_count; ++i)
                entry.shared_owners[i] = entry.shared_owners[i + 1];
            entry.shared_count--;
            entry.shared_owners[entry.shared_count] = nullptr;
        }

        bool add_shared_owner(BsdFlockEntry &entry, file *owner)
        {
            if (find_shared_owner(entry, owner) >= 0)
                return true;
            if (entry.shared_count >= kMaxBsdFlockSharedOwners)
                return false;
            entry.shared_owners[entry.shared_count++] = owner;
            return true;
        }

        void cleanup_entry_if_empty(BsdFlockEntry &entry)
        {
            if (entry.exclusive_owner != nullptr || entry.shared_count != 0)
                return;
            entry.used = false;
            entry.path.clear();
        }

        BsdFlockEntry *find_flock_entry(const eastl::string &path)
        {
            for (auto &entry : g_bsd_flock_entries)
            {
                if (entry.used && entry.path == path)
                    return &entry;
            }
            return nullptr;
        }

        BsdFlockEntry *alloc_flock_entry(const eastl::string &path)
        {
            for (auto &entry : g_bsd_flock_entries)
            {
                if (!entry.used)
                {
                    entry.used = true;
                    entry.path = path;
                    entry.exclusive_owner = nullptr;
                    entry.shared_count = 0;
                    for (auto &owner : entry.shared_owners)
                        owner = nullptr;
                    return &entry;
                }
            }
            return nullptr;
        }

	        void release_owner_locked(file *owner)
	        {
	            for (auto &entry : g_bsd_flock_entries)
	            {
                if (!entry.used)
                    continue;

                if (entry.exclusive_owner == owner)
                    entry.exclusive_owner = nullptr;
                remove_shared_owner(entry, owner);
	                cleanup_entry_if_empty(entry);
	            }
	        }

	        off_t record_lock_end(const struct flock &lock);

	        bool posix_lock_ranges_overlap(const struct flock &existing_lock, const struct flock &new_lock)
	        {
	            off_t existing_start = existing_lock.l_start;
	            off_t existing_end = record_lock_end(existing_lock);
	            off_t new_start = new_lock.l_start;
	            off_t new_end = record_lock_end(new_lock);
	            return existing_start < new_end && new_start < existing_end;
	        }

	        off_t record_lock_end(const struct flock &lock)
	        {
	            if (lock.l_len == 0)
	            {
	                return kLockOpenEnd;
	            }
	            if (lock.l_start >= kLockOpenEnd - lock.l_len)
	            {
	                return kLockOpenEnd;
	            }
	            return lock.l_start + lock.l_len;
	        }

	        struct flock make_record_lock_slice(const struct flock &base, off_t start, off_t end)
	        {
	            struct flock slice = base;
	            slice.l_whence = SEEK_SET;
	            slice.l_start = start;
	            slice.l_len = end >= kLockOpenEnd ? 0 : (end - start);
	            return slice;
	        }

	        bool record_lock_can_merge(const struct flock &lhs, const struct flock &rhs)
	        {
	            if (lhs.l_type != rhs.l_type)
	            {
	                return false;
	            }
	            off_t lhs_end = record_lock_end(lhs);
	            off_t rhs_end = record_lock_end(rhs);
	            return lhs_end >= rhs.l_start && rhs_end >= lhs.l_start;
	        }

	        template <typename Entry, typename MatchFn, typename AllocFn, typename ClearFn, typename RestoreFn>
	        void replace_same_owner_record_locks_locked(Entry *entries, int entry_count,
	                                                    const eastl::string &path,
	                                                    const struct flock &request,
	                                                    MatchFn match_same_owner,
	                                                    AllocFn alloc_slot,
	                                                    ClearFn clear_entry,
	                                                    RestoreFn restore_identity)
	        {
	            eastl::vector<struct flock> preserved;
	            preserved.reserve(8);

	            off_t request_start = request.l_start;
	            off_t request_end = record_lock_end(request);
	            printf("[record-lock-debug] replace-same-owner begin path=%s request_start=%ld request_end=%ld\n",
	                   path.c_str(), static_cast<long>(request_start), static_cast<long>(request_end));
	            print_record_lock_range("replace-request", request);
	            for (int i = 0; i < entry_count; ++i)
	            {
	                Entry &entry = entries[i];
	                if (!entry.used || entry.path != path || !match_same_owner(entry))
	                {
	                    continue;
	                }
	                if (!posix_lock_ranges_overlap(entry.lock, request))
	                {
	                    continue;
	                }

	                off_t entry_start = entry.lock.l_start;
	                off_t entry_end = record_lock_end(entry.lock);
	                struct flock original = entry.lock;
	                printf("[record-lock-debug] replace-overlap slot=%d entry_start=%ld entry_end=%ld request_start=%ld request_end=%ld\n",
	                       i, static_cast<long>(entry_start), static_cast<long>(entry_end),
	                       static_cast<long>(request_start), static_cast<long>(request_end));
	                print_record_lock_range("replace-original", original);
	                clear_entry(entry);

	                if (entry_start < request_start)
	                {
	                    struct flock slice = make_record_lock_slice(original, entry_start, request_start);
	                    print_record_lock_range("preserve-left", slice);
	                    preserved.push_back(slice);
	                }
	                if (request_end < entry_end)
	                {
	                    struct flock slice = make_record_lock_slice(original, request_end, entry_end);
	                    print_record_lock_range("preserve-right", slice);
	                    preserved.push_back(slice);
	                }
	            }

	            for (const auto &slice : preserved)
	            {
	                Entry *slot = alloc_slot();
	                if (slot == nullptr)
	                {
	                    printf("[record-lock-debug] replace-preserve-drop path=%s reason=no-slot\n",
	                           path.c_str());
	                    continue;
	                }
	                slot->used = true;
	                slot->path = path;
	                restore_identity(*slot);
	                slot->lock = slice;
	                print_record_lock_range("preserve-installed", slot->lock);
	            }
	            printf("[record-lock-debug] replace-same-owner end path=%s preserved=%lu\n",
	                   path.c_str(), static_cast<unsigned long>(preserved.size()));
	        }

	        template <typename Entry, typename SameOwnerFn, typename ClearFn>
	        void merge_same_owner_record_locks_locked(Entry *entries, int entry_count,
	                                                  const eastl::string &path,
	                                                  SameOwnerFn same_owner,
	                                                  ClearFn clear_entry)
	        {
	            bool changed = true;
	            while (changed)
	            {
	                changed = false;
	                for (int i = 0; i < entry_count && !changed; ++i)
	                {
	                    Entry &lhs = entries[i];
	                    if (!lhs.used || lhs.path != path)
	                    {
	                        continue;
	                    }
	                    for (int j = i + 1; j < entry_count; ++j)
	                    {
	                        Entry &rhs = entries[j];
	                        if (!rhs.used || rhs.path != path || !same_owner(lhs, rhs))
	                        {
	                            continue;
	                        }
	                        if (!record_lock_can_merge(lhs.lock, rhs.lock))
	                        {
	                            continue;
	                        }

	                        off_t merged_start = lhs.lock.l_start < rhs.lock.l_start ? lhs.lock.l_start : rhs.lock.l_start;
	                        off_t lhs_end = record_lock_end(lhs.lock);
	                        off_t rhs_end = record_lock_end(rhs.lock);
	                        off_t merged_end = lhs_end > rhs_end ? lhs_end : rhs_end;
	                        printf("[record-lock-debug] merge slots=%d,%d merged_start=%ld merged_end=%ld\n",
	                               i, j, static_cast<long>(merged_start), static_cast<long>(merged_end));
	                        print_record_lock_range("merge-left", lhs.lock);
	                        print_record_lock_range("merge-right", rhs.lock);
	                        lhs.lock = make_record_lock_slice(lhs.lock, merged_start, merged_end);
	                        clear_entry(rhs);
	                        changed = true;
	                        break;
	                    }
	                }
	            }
	        }

        bool posix_lock_conflicts(const struct flock &existing_lock, int existing_pid,
                                  const struct flock &new_lock, int new_pid)
        {
            if (existing_lock.l_type == F_UNLCK || new_lock.l_type == F_UNLCK)
                return false;
            if (existing_pid == new_pid)
                return false;
            if (!posix_lock_ranges_overlap(existing_lock, new_lock))
                return false;

            return existing_lock.l_type == F_WRLCK || new_lock.l_type == F_WRLCK;
        }

	        void clear_posix_record_lock(PosixRecordLockEntry &entry)
	        {
	            entry.used = false;
	            entry.path.clear();
	            entry.pid = 0;
	            memset(&entry.lock, 0, sizeof(entry.lock));
	        }

	        void clear_posix_record_lock_wait(PosixRecordLockWaitEntry &entry)
	        {
	            entry.used = false;
	            entry.path.clear();
	            entry.waiter_pid = 0;
	            entry.blocked_by_pid = 0;
	            memset(&entry.lock, 0, sizeof(entry.lock));
	        }

	        void clear_ofd_record_lock(OfdRecordLockEntry &entry)
	        {
	            entry.used = false;
	            entry.path.clear();
	            entry.owner = nullptr;
	            memset(&entry.lock, 0, sizeof(entry.lock));
	        }

	        PosixRecordLockEntry *alloc_posix_record_lock_slot()
	        {
	            for (auto &entry : g_posix_record_locks)
	            {
	                if (!entry.used)
	                {
	                    return &entry;
	                }
	            }
	            return nullptr;
	        }

	        OfdRecordLockEntry *alloc_ofd_record_lock_slot()
	        {
	            for (auto &entry : g_ofd_record_locks)
	            {
	                if (!entry.used)
	                {
	                    return &entry;
	                }
	            }
	            return nullptr;
	        }

	        PosixRecordLockWaitEntry *find_posix_record_lock_waiter(int waiter_pid)
	        {
	            for (auto &entry : g_posix_record_lock_waiters)
	            {
	                if (entry.used && entry.waiter_pid == waiter_pid)
	                {
	                    return &entry;
	                }
	            }
	            return nullptr;
	        }

	        PosixRecordLockWaitEntry *alloc_posix_record_lock_waiter_slot()
	        {
	            for (auto &entry : g_posix_record_lock_waiters)
	            {
	                if (!entry.used)
	                {
	                    return &entry;
	                }
	            }
	            return nullptr;
	        }

	        void clear_posix_record_lock_wait_for_pid_locked(int waiter_pid)
	        {
	            PosixRecordLockWaitEntry *entry = find_posix_record_lock_waiter(waiter_pid);
	            if (entry != nullptr)
	            {
	                clear_posix_record_lock_wait(*entry);
	            }
	        }

	        const PosixRecordLockEntry *find_conflicting_posix_record_lock_locked(
	            const eastl::string &path, int pid, const struct flock &lock)
	        {
	            const PosixRecordLockEntry *best = nullptr;
	            for (const auto &entry : g_posix_record_locks)
	            {
	                if (!entry.used || entry.path != path)
	                {
	                    continue;
	                }
	                if (!posix_lock_conflicts(entry.lock, entry.pid, lock, pid))
	                {
	                    continue;
	                }
	                if (best == nullptr || entry.lock.l_start < best->lock.l_start)
	                {
	                    best = &entry;
	                }
	            }
	            return best;
	        }

	        bool posix_record_lock_wait_cycle_locked(int waiter_pid, int blocked_by_pid)
	        {
	            int current_pid = blocked_by_pid;
	            for (int depth = 0; depth < kMaxPosixRecordLockWaiters && current_pid != 0; ++depth)
	            {
	                if (current_pid == waiter_pid)
	                {
	                    return true;
	                }

	                PosixRecordLockWaitEntry *entry = find_posix_record_lock_waiter(current_pid);
	                if (entry == nullptr || entry->blocked_by_pid == 0)
	                {
	                    return false;
	                }
	                current_pid = entry->blocked_by_pid;
	            }
	            return false;
	        }
	    }

    void init_bsd_flock_table()
    {
        g_bsd_flock_lock.init("bsd_flock_table");
        for (auto &entry : g_bsd_flock_entries)
        {
            entry.used = false;
            entry.path.clear();
            entry.exclusive_owner = nullptr;
            entry.shared_count = 0;
            for (auto &owner : entry.shared_owners)
                owner = nullptr;
        }
	        g_posix_record_lock.init("posix_record_lock_table");
	        for (auto &entry : g_posix_record_locks)
	            clear_posix_record_lock(entry);
	        for (auto &entry : g_posix_record_lock_waiters)
	            clear_posix_record_lock_wait(entry);
	        for (auto &entry : g_ofd_record_locks)
	            clear_ofd_record_lock(entry);
	        g_bsd_flock_ready = true;
	    }

    int apply_bsd_flock(file *owner, int operation)
    {
        if (owner == nullptr)
            return syscall::SYS_EBADF;

        // LOCK_NB 是修饰位，真正的锁模式必须且只能是 SH/EX/UN 之一。
        int mode = operation & ~LOCK_NB;
        if ((operation & ~(LOCK_SH | LOCK_EX | LOCK_UN | LOCK_NB)) != 0 ||
            (mode != LOCK_SH && mode != LOCK_EX && mode != LOCK_UN))
        {
            return syscall::SYS_EINVAL;
        }

        if (!g_bsd_flock_ready)
            return syscall::SYS_EINVAL;

        const eastl::string &path = owner->backing_path();
        if (path.empty())
            return 0;

        g_bsd_flock_lock.acquire();

        if (mode == LOCK_UN)
        {
            release_owner_locked(owner);
            g_bsd_flock_lock.release();
            return 0;
        }

        BsdFlockEntry *entry = find_flock_entry(path);
        if (entry == nullptr)
        {
            entry = alloc_flock_entry(path);
            if (entry == nullptr)
            {
                g_bsd_flock_lock.release();
                return syscall::SYS_ENFILE;
            }
        }

        if (mode == LOCK_SH)
        {
            if (entry->exclusive_owner != nullptr && entry->exclusive_owner != owner)
            {
                g_bsd_flock_lock.release();
                return syscall::SYS_EAGAIN;
            }

            // 同一 open file description 从独占锁降级为共享锁。
            if (entry->exclusive_owner == owner)
                entry->exclusive_owner = nullptr;
            if (!add_shared_owner(*entry, owner))
            {
                g_bsd_flock_lock.release();
                return syscall::SYS_ENFILE;
            }
            g_bsd_flock_lock.release();
            return 0;
        }

        int owner_shared_index = find_shared_owner(*entry, owner);
        int other_shared_count = entry->shared_count - (owner_shared_index >= 0 ? 1 : 0);
        if ((entry->exclusive_owner != nullptr && entry->exclusive_owner != owner) || other_shared_count > 0)
        {
            g_bsd_flock_lock.release();
            return syscall::SYS_EAGAIN;
        }

        // 同一 open file description 从共享锁升级为独占锁。
        remove_shared_owner(*entry, owner);
        entry->exclusive_owner = owner;
        g_bsd_flock_lock.release();
        return 0;
    }

    void release_bsd_flock(file *owner)
    {
        if (!g_bsd_flock_ready || owner == nullptr)
            return;
        g_bsd_flock_lock.acquire();
        release_owner_locked(owner);
        g_bsd_flock_lock.release();
    }

	    int apply_posix_record_lock(file *owner, int pid, const struct flock &lock, int *conflict_pid)
	    {
	        if (!g_bsd_flock_ready)
	            return syscall::SYS_EINVAL;
	        if (owner == nullptr)
            return syscall::SYS_EBADF;

	        const eastl::string &path = owner->backing_path();
	        if (path.empty())
	            return 0;

	        printf("[record-lock-debug] apply begin pid=%d path=%s conflict_ptr=%p\n",
	               pid, path.c_str(), reinterpret_cast<uint64>(conflict_pid));
	        print_record_lock_range("apply-request", lock);
	        g_posix_record_lock.acquire();
	        if (lock.l_type == F_UNLCK)
	        {
	            // 解锁成功后，当前进程不再处于等待图里，避免后续 F_SETLKW 误判历史环。
	            clear_posix_record_lock_wait_for_pid_locked(pid);
	            // POSIX record lock 对同一 pid 的部分解锁需要做区间裁剪，不能整条直接丢掉。
	            replace_same_owner_record_locks_locked(
	                g_posix_record_locks, kMaxPosixRecordLocks, path, lock,
	                [pid](const PosixRecordLockEntry &entry)
	                { return entry.pid == pid; },
	                []() -> PosixRecordLockEntry *
	                { return alloc_posix_record_lock_slot(); },
	                [](PosixRecordLockEntry &entry)
	                { clear_posix_record_lock(entry); },
	                [pid](PosixRecordLockEntry &entry)
	                { entry.pid = pid; });
	            dump_posix_record_locks_locked(path, "after-unlock");
	            g_posix_record_lock.release();
	            printf("[record-lock-debug] apply return pid=%d ret=0 unlock\n", pid);
	            return 0;
	        }

	        const PosixRecordLockEntry *conflict = find_conflicting_posix_record_lock_locked(path, pid, lock);
	        if (conflict != nullptr)
	        {
	            printf("[record-lock-debug] apply-conflict pid=%d blocked_by=%d\n",
	                   pid, conflict->pid);
	            print_record_lock_range("apply-conflict-lock", conflict->lock);
	            if (conflict_pid != nullptr)
	            {
	                *conflict_pid = conflict->pid;
	            }
	            g_posix_record_lock.release();
	            printf("[record-lock-debug] apply return pid=%d ret=%d conflict\n",
	                   pid, syscall::SYS_EAGAIN);
	            return syscall::SYS_EAGAIN;
	        }

	        // POSIX record lock 归属进程而不是 fd；同 pid 的重叠部分按 Linux 语义拆分/覆盖。
	        replace_same_owner_record_locks_locked(
	            g_posix_record_locks, kMaxPosixRecordLocks, path, lock,
	            [pid](const PosixRecordLockEntry &entry)
	            { return entry.pid == pid; },
	            []() -> PosixRecordLockEntry *
	            { return alloc_posix_record_lock_slot(); },
	            [](PosixRecordLockEntry &entry)
	            { clear_posix_record_lock(entry); },
	            [pid](PosixRecordLockEntry &entry)
	            { entry.pid = pid; });

	        PosixRecordLockEntry *slot = alloc_posix_record_lock_slot();
	        if (slot != nullptr)
	        {
	            slot->used = true;
	            slot->path = path;
	            slot->pid = pid;
	            slot->lock = lock;
	            // 锁真正拿到以后，清掉上一次阻塞尝试残留的等待边。
	            clear_posix_record_lock_wait_for_pid_locked(pid);
	            merge_same_owner_record_locks_locked(
	                g_posix_record_locks, kMaxPosixRecordLocks, path,
	                [](const PosixRecordLockEntry &lhs, const PosixRecordLockEntry &rhs)
	                { return lhs.pid == rhs.pid; },
	                [](PosixRecordLockEntry &entry)
	                { clear_posix_record_lock(entry); });
	            dump_posix_record_locks_locked(path, "after-set");
	            g_posix_record_lock.release();
	            printf("[record-lock-debug] apply return pid=%d ret=0 set\n", pid);
	            return 0;
	        }

	        printf("[record-lock-debug] apply no-slot pid=%d path=%s\n", pid, path.c_str());
	        g_posix_record_lock.release();
	        return syscall::SYS_ENFILE;
	    }

	    int query_posix_record_lock(file *owner, int pid, struct flock &lock)
	    {
	        if (!g_bsd_flock_ready)
	            return syscall::SYS_EINVAL;
	        if (owner == nullptr)
	            return syscall::SYS_EBADF;

	        const eastl::string &path = owner->backing_path();
	        if (path.empty())
	        {
	            lock.l_type = F_UNLCK;
	            return 0;
	        }

	        printf("[record-lock-debug] query begin pid=%d path=%s\n", pid, path.c_str());
	        print_record_lock_range("query-request", lock);
	        g_posix_record_lock.acquire();
	        const PosixRecordLockEntry *best = find_conflicting_posix_record_lock_locked(path, pid, lock);
	        if (best == nullptr)
	        {
	            printf("[record-lock-debug] query-best none pid=%d path=%s\n", pid, path.c_str());
	        }
	        else
	        {
	            printf("[record-lock-debug] query-best pid=%d owner_pid=%d\n", pid, best->pid);
	            print_record_lock_range("query-best-lock", best->lock);
	        }
	        dump_posix_record_locks_locked(path, "during-query");
	        g_posix_record_lock.release();

	        if (best == nullptr)
	        {
	            lock.l_type = F_UNLCK;
	            print_record_lock_range("query-return-unlocked", lock);
	            return 0;
	        }

	        lock = best->lock;
	        lock.l_pid = best->pid;
	        print_record_lock_range("query-return-conflict", lock);
	        return 0;
	    }

	    int note_posix_record_lock_wait(file *owner, int pid, const struct flock &lock, int blocked_by_pid)
	    {
	        if (!g_bsd_flock_ready)
	        {
	            return syscall::SYS_EINVAL;
	        }
	        if (owner == nullptr)
	        {
	            return syscall::SYS_EBADF;
	        }

	        const eastl::string &path = owner->backing_path();
	        if (path.empty())
	        {
	            return 0;
	        }

	        g_posix_record_lock.acquire();
	        if (posix_record_lock_wait_cycle_locked(pid, blocked_by_pid))
	        {
	            g_posix_record_lock.release();
	            return syscall::SYS_EDEADLK;
	        }

	        PosixRecordLockWaitEntry *entry = find_posix_record_lock_waiter(pid);
	        if (entry == nullptr)
	        {
	            entry = alloc_posix_record_lock_waiter_slot();
	        }
	        if (entry == nullptr)
	        {
	            g_posix_record_lock.release();
	            return syscall::SYS_ENFILE;
	        }

	        entry->used = true;
	        entry->path = path;
	        entry->waiter_pid = pid;
	        entry->blocked_by_pid = blocked_by_pid;
	        entry->lock = lock;
	        g_posix_record_lock.release();
	        return 0;
	    }

    void release_posix_record_locks_for_path(const eastl::string &path, int pid)
    {
        if (!g_bsd_flock_ready || path.empty())
            return;

        g_posix_record_lock.acquire();
        clear_posix_record_lock_wait_for_pid_locked(pid);
        for (auto &entry : g_posix_record_locks)
        {
            if (entry.used && entry.pid == pid && entry.path == path)
                clear_posix_record_lock(entry);
        }
        g_posix_record_lock.release();
    }

	    void release_posix_record_locks_for_pid(int pid)
	    {
	        if (!g_bsd_flock_ready)
	            return;

        g_posix_record_lock.acquire();
        clear_posix_record_lock_wait_for_pid_locked(pid);
        for (auto &entry : g_posix_record_locks)
        {
            if (entry.used && entry.pid == pid)
                clear_posix_record_lock(entry);
	        }
	        g_posix_record_lock.release();
	    }

	    int apply_ofd_record_lock(file *owner, const struct flock &lock)
	    {
	        if (!g_bsd_flock_ready)
	        {
	            return syscall::SYS_EINVAL;
	        }
	        if (owner == nullptr)
	        {
	            return syscall::SYS_EBADF;
	        }

	        const eastl::string &path = owner->backing_path();
	        if (path.empty())
	        {
	            return 0;
	        }

	        g_posix_record_lock.acquire();
	        if (lock.l_type == F_UNLCK)
	        {
	            replace_same_owner_record_locks_locked(
	                g_ofd_record_locks, kMaxOfdRecordLocks, path, lock,
	                [owner](const OfdRecordLockEntry &entry)
	                { return entry.owner == owner; },
	                []() -> OfdRecordLockEntry *
	                { return alloc_ofd_record_lock_slot(); },
	                [](OfdRecordLockEntry &entry)
	                { clear_ofd_record_lock(entry); },
	                [owner](OfdRecordLockEntry &entry)
	                { entry.owner = owner; });
	            g_posix_record_lock.release();
	            return 0;
	        }

	        for (const auto &entry : g_ofd_record_locks)
	        {
	            if (!entry.used || entry.path != path)
	            {
	                continue;
	            }
	            if (entry.owner == owner)
	            {
	                continue;
	            }
	            if (!posix_lock_ranges_overlap(entry.lock, lock))
	            {
	                continue;
	            }
	            if (entry.lock.l_type == F_WRLCK || lock.l_type == F_WRLCK)
	            {
	                g_posix_record_lock.release();
	                return syscall::SYS_EAGAIN;
	            }
	        }

	        replace_same_owner_record_locks_locked(
	            g_ofd_record_locks, kMaxOfdRecordLocks, path, lock,
	            [owner](const OfdRecordLockEntry &entry)
	            { return entry.owner == owner; },
	            []() -> OfdRecordLockEntry *
	            { return alloc_ofd_record_lock_slot(); },
	            [](OfdRecordLockEntry &entry)
	            { clear_ofd_record_lock(entry); },
	            [owner](OfdRecordLockEntry &entry)
	            { entry.owner = owner; });

	        OfdRecordLockEntry *slot = alloc_ofd_record_lock_slot();
	        if (slot != nullptr)
	        {
	            slot->used = true;
	            slot->path = path;
	            slot->owner = owner;
	            slot->lock = lock;
	            merge_same_owner_record_locks_locked(
	                g_ofd_record_locks, kMaxOfdRecordLocks, path,
	                [](const OfdRecordLockEntry &lhs, const OfdRecordLockEntry &rhs)
	                { return lhs.owner == rhs.owner; },
	                [](OfdRecordLockEntry &entry)
	                { clear_ofd_record_lock(entry); });
	            g_posix_record_lock.release();
	            return 0;
	        }

	        g_posix_record_lock.release();
	        return syscall::SYS_ENFILE;
	    }

	    int query_ofd_record_lock(file *owner, struct flock &lock)
	    {
	        if (!g_bsd_flock_ready)
	        {
	            return syscall::SYS_EINVAL;
	        }
	        if (owner == nullptr)
	        {
	            return syscall::SYS_EBADF;
	        }

	        const eastl::string &path = owner->backing_path();
	        if (path.empty())
	        {
	            lock.l_type = F_UNLCK;
	            return 0;
	        }

	        g_posix_record_lock.acquire();
	        const OfdRecordLockEntry *best = nullptr;
	        for (const auto &entry : g_ofd_record_locks)
	        {
	            if (!entry.used || entry.path != path || entry.owner == owner)
	            {
	                continue;
	            }
	            if (!posix_lock_ranges_overlap(entry.lock, lock))
	            {
	                continue;
	            }
	            if (entry.lock.l_type != F_WRLCK && lock.l_type != F_WRLCK)
	            {
	                continue;
	            }
	            if (best == nullptr || entry.lock.l_start < best->lock.l_start)
	            {
	                best = &entry;
	            }
	        }
	        g_posix_record_lock.release();

	        if (best == nullptr)
	        {
	            lock.l_type = F_UNLCK;
	            return 0;
	        }

	        lock = best->lock;
	        lock.l_pid = -1;
	        return 0;
	    }

	    void release_ofd_record_locks_for_owner(file *owner)
	    {
	        if (!g_bsd_flock_ready || owner == nullptr)
	        {
	            return;
	        }

	        g_posix_record_lock.acquire();
	        for (auto &entry : g_ofd_record_locks)
	        {
	            if (entry.used && entry.owner == owner)
	            {
	                clear_ofd_record_lock(entry);
	            }
	        }
	        g_posix_record_lock.release();
	    }

    void file_pool::init()
    {
        _lock.init("file pool");
        for (auto &f : _files)
        { // refcnt 的初始化在构造参数中
            // f.ref = 0;
            f.type = fs::FileTypes::FT_NONE;
        }
    }

    File *file_pool::alloc_file()
    {
        _lock.acquire();
        for (auto &f : _files)
        {
            if (f.refcnt == 0 && f.type == FileTypes::FT_NONE)
            {
                f.refcnt = 1;
                _lock.release();
                return &f;
            }
        }
        _lock.release();
        return nullptr;
    }

    void file_pool::free_file(File *f)
    {
        _lock.acquire();
        if (f->refcnt <= 0)
        {
            printfRed("[file pool] free no-ref file");
            _lock.release();
            return;
        }
        --f->refcnt;
        if (f->refcnt == 0)
        {
            if (f->type == FileTypes::FT_PIPE)
                // f->pipe->close( f->writable );
                f->data.get_Pipe()->close(f->ops.fields.w);
            f->type = FileTypes::FT_NONE;
            f->flags = 0;
            f->ops = FileOps(0);
            // Placement new
            new (&f->data) File::Data(FileTypes::FT_NONE);
        }
        _lock.release();
    }

    void file_pool::dup(File *f)
    {
        _lock.acquire();
        assert(f->refcnt >= 1, "file: try to dup no reference file.");
        f->refcnt++;
        _lock.release();
    }

    File *file_pool::find_file(eastl::string path)
    {
        panic("file_pool::find_file: not implemented yet");
        // _lock.acquire();
        // for (auto &f : _files)
        // {
        //     dentry *den = f.data.get_Entry();
        //     if (den && den->rName() == path)
        //     {
        //         _lock.release();
        //         return &f;
        //     }
        // }
        // _lock.release();
        return nullptr;
    }

    int file_pool::unlink(eastl::string path)
    {
        _lock.acquire();
        _unlink_list.push_back(path);
        _lock.release();
        return 0;
    }
    void file_pool::remove(eastl::string path)
    {
        _lock.acquire();
        _unlink_list.erase_first(path);
        _lock.release();
    }

    bool file_pool::has_unlinked(const eastl::string &path)
    {
        _lock.acquire();
        bool found = eastl::find(_unlink_list.begin(), _unlink_list.end(), path) != _unlink_list.end();
        _lock.release();
        return found;
    }
}

#include "fs/vfs/file/file.hh"

#include "proc.hh"
#include "proc_manager.hh"

#include "klib.hh"
#include "types.hh"
#include "syscall_defs.hh"

#include <EASTL/string.h>

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

        struct BsdFlockEntry
        {
            bool used = false;
            eastl::string path;
            file *exclusive_owner = nullptr;
            file *shared_owners[kMaxBsdFlockSharedOwners] = {};
            int shared_count = 0;
        };

        SpinLock g_bsd_flock_lock;
        BsdFlockEntry g_bsd_flock_entries[kMaxBsdFlockEntries];
        bool g_bsd_flock_ready = false;

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
        _unlink_list.erase_first(path);
    }
}

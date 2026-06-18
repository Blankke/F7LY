#pragma once

#include "types.hh"
#include "devs/spinlock.hh"
#include "shm/ipc_param.hh"
#include "vm_area.hh"
#include <EASTL/atomic.h>
#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>

namespace fs
{
    class file;
}

namespace proc
{
    enum class VmObjectKind : uint8
    {
        Anon = 0,
        File = 1,
        SysvShm = 2,
    };

    struct VmPageView
    {
        uint64 pa = 0;
        bool writable = false;
        bool mark_cow = false;
        bool private_overlay = false;
    };

    class VmObject
    {
    public:
        explicit VmObject(VmObjectKind kind, bool shared_mapping);
        virtual ~VmObject();

        VmObject(const VmObject &) = delete;
        VmObject &operator=(const VmObject &) = delete;

        void get();
        bool put();
        int ref_count_for_debug() const;

        VmObjectKind kind() const { return object_kind_; }
        uint64 object_id() const { return object_id_; }
        bool shared_mapping() const { return shared_mapping_; }

        virtual int prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view) = 0;
        virtual int sync_area_range(const VmArea &area, uint64 start, uint64 end) { return 0; }
        virtual void on_area_destroy(VmArea &area);

        virtual fs::file *backing_file() const { return nullptr; }
        virtual int shmid() const { return -1; }
        virtual const eastl::string *shared_cache_key() const { return nullptr; }

    protected:
        uint64 ensure_source_page(uint64 key, bool zero_fill);
        uint64 find_source_page(uint64 key) const;
        uint64 allocate_private_overlay_page(VmArea &area, uint64 page_index, const void *src, size_t copy_bytes, bool zero_fill_tail);
        void release_source_pages();

        mutable SpinLock object_lock_;
        eastl::unordered_map<uint64, uint64> source_pages_;

    private:
        uint64 object_id_;
        VmObjectKind object_kind_;
        bool shared_mapping_;
        eastl::atomic<int> ref_count_;
    };

    class AnonVmObject final : public VmObject
    {
    public:
        explicit AnonVmObject(bool shared_mapping, const char *debug_name = nullptr);
        ~AnonVmObject() override;

        int prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view) override;

    private:
        const char *debug_name_;
    };

    class FileVmObject final : public VmObject
    {
    public:
        FileVmObject(fs::file *file, bool shared_mapping, bool zero_fill_past_file, const eastl::string &cache_key = {});
        ~FileVmObject() override;

        int prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view) override;
        int sync_area_range(const VmArea &area, uint64 start, uint64 end) override;
        fs::file *backing_file() const override { return file_; }
        const eastl::string &cache_key() const { return cache_key_; }
        const eastl::string *shared_cache_key() const override
        {
            return shared_mapping() && !cache_key_.empty() ? &cache_key_ : nullptr;
        }

    private:
        fs::file *file_;
        bool zero_fill_past_file_;
        eastl::string cache_key_;
    };

    struct SysvShmMetadata
    {
        int shmid = -1;
        uint64 ipc_ns_id = 0;
        key_t key = 0;
        size_t size = 0;
        size_t real_size = 0;
        uint16 shmflg = 0;
        time_t atime = 0;
        time_t dtime = 0;
        time_t ctime = 0;
        pid_t creator_pid = 0;
        pid_t last_pid = 0;
        uid_t owner_uid = 0;
        gid_t owner_gid = 0;
        uid_t creator_uid = 0;
        gid_t creator_gid = 0;
        mode_t mode = 0;
        int nattch = 0;
        bool auto_destroy_on_last_detach = false;
        unsigned short seq = 0;
    };

    class SysvShmVmObject final : public VmObject
    {
    public:
        explicit SysvShmVmObject(const SysvShmMetadata &meta);
        ~SysvShmVmObject() override;

        int prepare_page(VmArea &area, uint64 page_index, int access_type, VmPageView &view) override;
        int shmid() const override { return meta_.shmid; }

        SysvShmMetadata meta_;
    };
}

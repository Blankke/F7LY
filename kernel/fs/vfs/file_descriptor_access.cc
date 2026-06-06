#include "fs/vfs/file_descriptor_access.hh"

#include "fs/vfs/file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "proc.hh"
#include "virtual_memory_manager.hh"
#include <EASTL/vector.h>
#include <asm-generic/errno-base.h>

namespace fs
{
    namespace
    {
        constexpr uint64 k_direct_io_alignment = 512;
    }

    bool FileDescriptorAccess::access_mode_has_write(int flags)
    {
        int accmode = flags & O_ACCMODE;
        return accmode == O_WRONLY || accmode == O_RDWR;
    }

    bool FileDescriptorAccess::allows_read(const file *f)
    {
        if (f == nullptr)
        {
            return false;
        }
        if (f->is_fanotify_file() || f->is_inotify_file())
        {
            return true;
        }

        switch (f->_attrs.filetype)
        {
        case FT_NORMAL:
        case FT_DIRECT:
        case FT_SYMLINK:
            return (f->lwext4_file_struct.flags & O_ACCMODE) != O_WRONLY;
        case FT_DEVICE:
            if (f->is_virtual)
            {
                return (f->lwext4_file_struct.flags & O_ACCMODE) != O_WRONLY;
            }
            return f->_attrs.g_read != 0;
        case FT_PIPE:
            return static_cast<const pipe_file *>(f)->allows_read_end();
        case FT_SOCKET:
            return f->_attrs.g_read != 0;
        default:
            return f->is_virtual ? ((f->lwext4_file_struct.flags & O_ACCMODE) != O_WRONLY)
                                 : (f->_attrs.g_read != 0);
        }
    }

    bool FileDescriptorAccess::allows_write(const file *f)
    {
        if (f == nullptr)
        {
            return false;
        }

        switch (f->_attrs.filetype)
        {
        case FT_NORMAL:
        case FT_DIRECT:
        case FT_SYMLINK:
            return (f->lwext4_file_struct.flags & O_ACCMODE) != O_RDONLY;
        case FT_DEVICE:
            if (f->is_virtual)
            {
                return (f->lwext4_file_struct.flags & O_ACCMODE) != O_RDONLY;
            }
            return f->_attrs.g_write != 0;
        case FT_PIPE:
            return static_cast<const pipe_file *>(f)->allows_write_end();
        case FT_SOCKET:
            return f->_attrs.g_write != 0;
        default:
            return f->is_virtual ? ((f->lwext4_file_struct.flags & O_ACCMODE) != O_RDONLY)
                                 : (f->_attrs.g_write != 0);
        }
    }

    OpenDescriptionStats FileDescriptorAccess::collect_open_description_stats(const eastl::string &path,
                                                                              file *self)
    {
        OpenDescriptionStats stats{};
        eastl::vector<file *> seen;
        seen.reserve(proc::num_process);

        for (uint i = 0; i < proc::num_process; ++i)
        {
            proc::Pcb *pcb = &proc::k_proc_pool[i];
            if (pcb->_state == proc::ProcState::UNUSED || pcb->_ofile == nullptr)
            {
                continue;
            }

            for (uint fd = 0; fd < proc::max_open_files; ++fd)
            {
                file *candidate = pcb->_ofile->_ofile_ptr[fd];
                if (candidate == nullptr || candidate->backing_path() != path)
                {
                    continue;
                }

                bool already_seen = false;
                for (file *existing : seen)
                {
                    if (existing == candidate)
                    {
                        already_seen = true;
                        break;
                    }
                }
                if (already_seen)
                {
                    continue;
                }

                seen.push_back(candidate);
                stats.distinct_description_count++;
                if (access_mode_has_write(candidate->lwext4_file_struct.flags))
                {
                    stats.has_writable_description = true;
                }
                if (candidate != self && candidate->_lease_type != F_UNLCK)
                {
                    stats.has_other_lease_owner = true;
                }
            }
        }

        return stats;
    }

    int FileDescriptorAccess::validate_direct_io_request(file *f,
                                                         mem::PageTable &pt,
                                                         uint64 user_buffer,
                                                         size_t length,
                                                         long offset,
                                                         bool kernel_writes_user_buffer)
    {
        if (f == nullptr ||
            (f->lwext4_file_struct.flags & O_DIRECT) == 0 ||
            f->_attrs.filetype != FileTypes::FT_NORMAL)
        {
            return 0;
        }
        if (offset < 0 ||
            (user_buffer % k_direct_io_alignment) != 0 ||
            (length % k_direct_io_alignment) != 0 ||
            (static_cast<uint64>(offset) % k_direct_io_alignment) != 0)
        {
            return -EINVAL;
        }
        if (length == 0)
        {
            return 0;
        }

        int access_result = kernel_writes_user_buffer
                                ? mem::k_vmm.ensure_user_write_range(pt, user_buffer, length)
                                : mem::k_vmm.ensure_user_read_range(pt, user_buffer, length);
        return access_result == 0 ? 0 : -EFAULT;
    }
}

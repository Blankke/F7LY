#include "sysio.hh"

#include "fs/vfs/file/file.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/virtual_memory_manager.hh"
#include "syscall_defs.hh"

namespace syscall
{
    namespace
    {
        constexpr long k_direct_user_io_unimplemented = -38;
        constexpr long k_direct_user_read_unimplemented = k_direct_user_io_unimplemented;
        constexpr long k_direct_user_write_unimplemented = k_direct_user_io_unimplemented;

        size_t syscall_iovec_buffer_size(const KernelIovec *iovecs, int iovcnt)
        {
            size_t buffer_size = 1;
            for (int i = 0; i < iovcnt; ++i)
            {
                size_t need = min_size(iovecs[i].len, k_syscall_io_chunk_size);
                if (need > buffer_size)
                {
                    buffer_size = need;
                }
            }
            return buffer_size;
        }
    }

    void *alloc_syscall_temp_buffer(size_t size)
    {
        if (size == 0)
        {
            size = 1;
        }
        return mem::k_pmm.kmalloc(size);
    }

    void free_syscall_temp_buffer(void *ptr)
    {
        if (ptr != nullptr)
        {
            mem::k_pmm.free_page(ptr);
        }
    }

    ScopedKernelIovecArray::ScopedKernelIovecArray(int count)
    {
        if (count <= k_syscall_iovec_inline_count)
        {
            data_ = inline_iovecs_;
            return;
        }

        size_t bytes = sizeof(KernelIovec) * static_cast<size_t>(count);
        data_ = static_cast<KernelIovec *>(alloc_syscall_temp_buffer(bytes));
        heap_backed_ = true;
    }

    ScopedKernelIovecArray::~ScopedKernelIovecArray()
    {
        if (heap_backed_)
        {
            free_syscall_temp_buffer(data_);
        }
    }

    ScopedSyscallBuffer::ScopedSyscallBuffer(size_t size)
    {
        ensure(size);
    }

    ScopedSyscallBuffer::~ScopedSyscallBuffer()
    {
        if (heap_backed_)
        {
            free_syscall_temp_buffer(data_);
        }
    }

    bool ScopedSyscallBuffer::ensure(size_t size)
    {
        if (data_ != nullptr)
        {
            return true;
        }
        if (size == 0)
        {
            size = 1;
        }

        if (size <= k_syscall_io_inline_buffer_size)
        {
            data_ = inline_buffer_;
            heap_backed_ = false;
        }
        else
        {
            data_ = static_cast<char *>(alloc_syscall_temp_buffer(size));
            heap_backed_ = true;
        }
        return data_ != nullptr;
    }

    int copy_user_iovecs(mem::PageTable &pt, uint64 iov_ptr, int iovcnt, KernelIovec *iovecs, size_t *total_len)
    {
        struct UserIovec
        {
            uint64 iov_base;
            size_t iov_len;
        };

        size_t bytes = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            UserIovec user_iov{};
            uint64 user_iov_addr = iov_ptr + static_cast<uint64>(i) * sizeof(UserIovec);
            if (mem::k_vmm.copy_in(pt, &user_iov, user_iov_addr, sizeof(user_iov)) < 0)
            {
                return SYS_EFAULT;
            }
            if (user_iov.iov_len > static_cast<size_t>(0x7FFFFFFF) - bytes)
            {
                return SYS_EINVAL;
            }

            iovecs[i].base = user_iov.iov_base;
            iovecs[i].len = user_iov.iov_len;
            bytes += user_iov.iov_len;
        }

        if (total_len != nullptr)
        {
            *total_len = bytes;
        }
        return 0;
    }

    long write_from_user_iovecs(fs::file *f, mem::PageTable &pt, const KernelIovec *iovecs, int iovcnt, long *explicit_off)
    {
        ScopedSyscallBuffer buffer;
        size_t fallback_buffer_size = 0;
        long total_written = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            size_t iov_done = 0;
            while (iov_done < iovecs[i].len)
            {
                size_t want = min_size(iovecs[i].len - iov_done, k_syscall_io_chunk_size);
                long write_off = explicit_off == nullptr ? -1 : *explicit_off;
                bool upgrade = explicit_off == nullptr;
                long rc = f->write_from_user(pt, iovecs[i].base + iov_done, want, write_off, upgrade);
                if (rc == k_direct_user_write_unimplemented)
                {
                    if (fallback_buffer_size == 0)
                    {
                        fallback_buffer_size = syscall_iovec_buffer_size(iovecs, iovcnt);
                    }
                    if (!buffer.ensure(fallback_buffer_size))
                    {
                        return total_written > 0 ? total_written : SYS_ENOMEM;
                    }
                    if (mem::k_vmm.copy_in(pt, buffer.data(), iovecs[i].base + iov_done, want) < 0)
                    {
                        return total_written > 0 ? total_written : SYS_EFAULT;
                    }

                    rc = f->write(reinterpret_cast<ulong>(buffer.data()), want, write_off, upgrade);
                }
                if (rc < 0)
                {
                    return total_written > 0 ? total_written : rc;
                }
                if (rc == 0)
                {
                    return total_written;
                }

                total_written += rc;
                iov_done += static_cast<size_t>(rc);
                if (explicit_off != nullptr)
                {
                    *explicit_off += rc;
                }
                if (static_cast<size_t>(rc) < want)
                {
                    return total_written;
                }
            }
        }

        return total_written;
    }

    long read_to_user_iovecs(fs::file *f, mem::PageTable &pt, const KernelIovec *iovecs, int iovcnt, long *explicit_off)
    {
        long total_read = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            size_t iov_done = 0;
            while (iov_done < iovecs[i].len)
            {
                size_t want = min_size(iovecs[i].len - iov_done, k_syscall_io_chunk_size);
                long read_off = explicit_off == nullptr ? -1 : *explicit_off;
                bool upgrade = explicit_off == nullptr;
                long rc = f->read_to_user(pt, iovecs[i].base + iov_done, want, read_off, upgrade);
                if (rc == k_direct_user_read_unimplemented)
                {
                    // 普通文件已经有直接 copy_out 快路径；未支持快路径的文件仍走内核中转。
                    ScopedSyscallBuffer buffer(want);
                    if (!buffer.valid())
                    {
                        return total_read > 0 ? total_read : SYS_ENOMEM;
                    }

                    rc = f->read(reinterpret_cast<uint64>(buffer.data()), want, read_off, upgrade);
                    if (rc > 0 &&
                        mem::k_vmm.copy_out(pt, iovecs[i].base + iov_done, buffer.data(), rc) < 0)
                    {
                        return total_read > 0 ? total_read : SYS_EFAULT;
                    }
                }
                if (rc < 0)
                {
                    return total_read > 0 ? total_read : rc;
                }
                if (rc == 0)
                {
                    return total_read;
                }

                total_read += rc;
                iov_done += static_cast<size_t>(rc);
                if (explicit_off != nullptr)
                {
                    *explicit_off += rc;
                }
                if (static_cast<size_t>(rc) < want)
                {
                    return total_read;
                }
            }
        }

        return total_read;
    }
}

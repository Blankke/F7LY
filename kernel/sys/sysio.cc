#include "sysio.hh"

#include "fs/vfs/file/file.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/virtual_memory_manager.hh"
#include "libs/perf_diag.hh"
#include "param.h"
#include "syscall_defs.hh"

namespace syscall
{
    namespace
    {
        constexpr long k_direct_user_io_unimplemented = -38;
        constexpr long k_direct_user_read_unimplemented = k_direct_user_io_unimplemented;
        constexpr long k_direct_user_write_unimplemented = k_direct_user_io_unimplemented;

        struct alignas(64) SyscallIoPoolSlot
        {
            // 0=空闲，1=已租出。租约不绑定 CPU，任务持有期间即使迁移或
            // 嵌套进入另一条 I/O 路径，也不会让同一槽被并发复用。
            uint32 leased = 0;
            char *data = nullptr;
        };

        SyscallIoPoolSlot g_syscall_io_pool[NUMCPU]{};

        char *acquire_syscall_io_pool_slot(int &slot_index)
        {
            slot_index = -1;
            for (int index = 0; index < NUMCPU; ++index)
            {
                uint32 expected = 0;
                if (__atomic_compare_exchange_n(&g_syscall_io_pool[index].leased,
                                                &expected,
                                                1,
                                                false,
                                                __ATOMIC_ACQUIRE,
                                                __ATOMIC_RELAXED))
                {
                    if (g_syscall_io_pool[index].data == nullptr)
                    {
                        // 槽首次使用时永久保留一块 64 KiB 未清零缓冲；租约
                        // 已独占该槽，因此初始化指针无需第二把锁。
                        g_syscall_io_pool[index].data = static_cast<char *>(
                            mem::k_pmm.kmalloc_uninitialized(k_syscall_io_chunk_size));
                        if (g_syscall_io_pool[index].data == nullptr)
                        {
                            __atomic_store_n(&g_syscall_io_pool[index].leased,
                                             0,
                                             __ATOMIC_RELEASE);
                            continue;
                        }
                        F7LY_PERF_ADD(SysIoTempAlloc, 1);
                    }
                    slot_index = index;
                    return g_syscall_io_pool[index].data;
                }
            }
            return nullptr;
        }

        void release_syscall_io_pool_slot(int slot_index, char *data)
        {
            if (slot_index < 0 || slot_index >= NUMCPU ||
                data != g_syscall_io_pool[slot_index].data)
            {
                panic("sysio: invalid pooled buffer release slot=%d data=%p",
                      slot_index, data);
            }

            const uint32 previous =
                __atomic_exchange_n(&g_syscall_io_pool[slot_index].leased,
                                    0,
                                    __ATOMIC_RELEASE);
            if (previous != 1)
            {
                panic("sysio: double pooled buffer release slot=%d data=%p",
                      slot_index, data);
            }
        }

        void record_syscall_temp_buffer_bytes(size_t requested_size)
        {
            // bytes 记录实际租用的有效容量，不把池保留的 64 KiB 重复计入；
            // TempAlloc 则只统计真正发生的 PMM 动态分配。
            F7LY_PERF_ADD(SysIoTempBytes, requested_size);
        }

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
        void *buffer = mem::k_pmm.kmalloc(size);
        if (buffer != nullptr)
        {
            F7LY_PERF_ADD(SysIoTempAlloc, 1);
            record_syscall_temp_buffer_bytes(size);
        }
        return buffer;
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
        if (pool_slot_ >= 0)
        {
            release_syscall_io_pool_slot(pool_slot_, data_);
        }
        else if (heap_backed_)
        {
            free_syscall_temp_buffer(data_);
        }
    }

    bool ScopedSyscallBuffer::ensure(size_t size)
    {
        if (size == 0)
        {
            size = 1;
        }
        if (data_ != nullptr)
        {
            // ensure 的成功必须真的覆盖请求容量，不能让后续完整 copy_in/read
            // 因复用一个更小的旧缓冲而越界。
            return size <= capacity_;
        }

        if (size <= k_syscall_io_inline_buffer_size)
        {
            data_ = inline_buffer_;
            capacity_ = k_syscall_io_inline_buffer_size;
            heap_backed_ = false;
        }
        else
        {
            bool attempted_pool = false;
            if (size <= k_syscall_io_chunk_size)
            {
                attempted_pool = true;
                data_ = acquire_syscall_io_pool_slot(pool_slot_);
            }
            if (data_ == nullptr)
            {
                if (attempted_pool)
                {
                    F7LY_PERF_ADD(SysIoPoolMiss, 1);
                }
                // 完整 copy_in 或底层 read 会覆盖随后使用的有效区间；池耗尽
                // 时也不应为 64 KiB 中转页支付无意义的预清零成本。
                data_ = static_cast<char *>(mem::k_pmm.kmalloc_uninitialized(size));
                heap_backed_ = data_ != nullptr;
                pool_slot_ = -1;
                if (data_ != nullptr)
                {
                    F7LY_PERF_ADD(SysIoTempAlloc, 1);
                }
            }
            else
            {
                F7LY_PERF_ADD(SysIoPoolHit, 1);
                capacity_ = k_syscall_io_chunk_size;
            }
            if (data_ != nullptr)
            {
                if (pool_slot_ < 0)
                {
                    capacity_ = size;
                }
                record_syscall_temp_buffer_bytes(size);
            }
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
                if (rc > static_cast<long>(want))
                {
                    // read 路径同样校验这一底层契约；write 若越界报告完成量，
                    // 会让 iov 游标越过用户提供的范围并破坏显式 offset。
                    return total_written > 0 ? total_written : SYS_EIO;
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
                    if (rc > static_cast<long>(want))
                    {
                        // 下层绝不能报告超过传入容量的完成字节；否则即使只按
                        // rc copy_out，也会把未初始化页尾或相邻内存暴露给用户。
                        return total_read > 0 ? total_read : SYS_EIO;
                    }
                    if (rc > 0 &&
                        mem::k_vmm.copy_out(pt, iovecs[i].base + iov_done, buffer.data(), rc) < 0)
                    {
                        return total_read > 0 ? total_read : SYS_EFAULT;
                    }
                }
                else if (rc > static_cast<long>(want))
                {
                    // 直接 copy_out 实现也必须遵守与 fallback 相同的完成量契约。
                    return total_read > 0 ? total_read : SYS_EIO;
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

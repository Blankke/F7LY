#pragma once

#include "types.hh"

namespace fs
{
    class file;
}

namespace mem
{
    class PageTable;
}

namespace syscall
{
    constexpr size_t k_syscall_io_chunk_size = 64 * 1024;
    constexpr size_t k_syscall_io_inline_buffer_size = 2 * 1024;
    constexpr int k_syscall_iovec_inline_count = 16;

    inline size_t min_size(size_t lhs, size_t rhs)
    {
        return lhs < rhs ? lhs : rhs;
    }

    void *alloc_syscall_temp_buffer(size_t size);
    void free_syscall_temp_buffer(void *ptr);

    struct KernelIovec
    {
        uint64 base;
        size_t len;
    };

    class ScopedKernelIovecArray
    {
    public:
        explicit ScopedKernelIovecArray(int count);
        ~ScopedKernelIovecArray();

        bool valid() const { return data_ != nullptr; }
        KernelIovec *data() { return data_; }

    private:
        KernelIovec inline_iovecs_[k_syscall_iovec_inline_count];
        KernelIovec *data_ = nullptr;
        bool heap_backed_ = false;
    };

    class ScopedSyscallBuffer
    {
    public:
        ScopedSyscallBuffer() = default;
        explicit ScopedSyscallBuffer(size_t size);
        ~ScopedSyscallBuffer();

        bool valid() const { return data_ != nullptr; }
        bool ensure(size_t size);
        char *data() { return data_; }

    private:
        alignas(16) char inline_buffer_[k_syscall_io_inline_buffer_size];
        char *data_ = nullptr;
        bool heap_backed_ = false;
    };

    int copy_user_iovecs(mem::PageTable &pt, uint64 iov_ptr, int iovcnt, KernelIovec *iovecs, size_t *total_len);
    long write_from_user_iovecs(fs::file *f, mem::PageTable &pt, const KernelIovec *iovecs, int iovcnt, long *explicit_off);
    long read_to_user_iovecs(fs::file *f, mem::PageTable &pt, const KernelIovec *iovecs, int iovcnt, long *explicit_off);
}

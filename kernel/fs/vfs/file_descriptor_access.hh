#pragma once

#include "types.hh"
#include <EASTL/string.h>

namespace mem
{
    class PageTable;
}

namespace fs
{
    class file;

    struct OpenDescriptionStats
    {
        int distinct_description_count = 0;
        bool has_writable_description = false;
        bool has_other_lease_owner = false;
    };

    class FileDescriptorAccess
    {
    public:
        // 文件描述符读写权限、O_DIRECT 对齐校验、lease 打开描述统计都属于
        // VFS/FD 语义，syscall_handler 只调用这里的判定结果。
        static bool access_mode_has_write(int flags);
        static bool allows_read(const file *file);
        static bool allows_write(const file *file);
        static OpenDescriptionStats collect_open_description_stats(const eastl::string &path,
                                                                   file *self);
        static int validate_direct_io_request(file *file,
                                              mem::PageTable &pt,
                                              uint64 user_buffer,
                                              size_t length,
                                              long offset,
                                              bool kernel_writes_user_buffer);
    };
}

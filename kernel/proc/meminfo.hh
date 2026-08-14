#pragma once

#include <EASTL/string.h>

#include "mem/physical_memory_manager.hh"

inline eastl::string meminfo_unsigned_decimal(uint64 value)
{
    eastl::string result;
    char digits[32];
    int length = 0;
    do
    {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (length > 0)
    {
        result += digits[--length];
    }
    return result;
}

inline eastl::string get_meminfo()
{
    // /proc/meminfo 必须反映 DTB/PMM 的实际容量，不能继续保留固定的
    // 固定容量字符串，否则资源感知型用户程序会错误判断内存上限。
    const uint64 total_kb =
        static_cast<uint64>(mem::k_pmm.get_page_count()) * PGSIZE / 1024;
    const uint64 free_kb =
        mem::k_pmm.get_free_page_count() * PGSIZE / 1024;
    const eastl::string total = meminfo_unsigned_decimal(total_kb) + " kB";
    const eastl::string free = meminfo_unsigned_decimal(free_kb) + " kB";
    eastl::string result;

    result += "MemTotal: " + total + "\n";
    result += "MemFree: " + free + "\n";
    result += "MemAvailable: " + free + "\n";
    result += "Buffers: 0 kB\n";
    result += "Cached: 0 kB\n";
    result += "SwapCached: 0 kB\n";
    result += "SwapTotal: 0 kB\n";
    result += "SwapFree: 0 kB\n";
    result += "Shmem: 0 kB\n";
    result += "Slab: 0 kB\n";

    return result;
}

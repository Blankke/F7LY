#pragma once
#include <EASTL/string.h>

struct meminfo
{
    eastl::string MemTotal = "16773120 kB";
    eastl::string MemFree = "327680 kB";
    eastl::string MemAvailable = "16445440 kB";
    eastl::string Buffers = "373336 kB";
    eastl::string Cached = "10391984 kB";
    eastl::string SwapCached = "0 kB";
    eastl::string SwapTotal = "0 kB";
    eastl::string SwapFree = "0 kB";
    eastl::string Shmem = "0 kB";
    eastl::string Slab = "0 kB";
};

inline eastl::string get_meminfo()
{
    meminfo info;
    eastl::string result;

    result += "MemTotal: " + info.MemTotal + "\n";
    result += "MemFree: " + info.MemFree + "\n";
    result += "MemAvailable: " + info.MemAvailable + "\n";
    result += "Buffers: " + info.Buffers + "\n";
    result += "Cached: " + info.Cached + "\n";
    result += "SwapCached: " + info.SwapCached + "\n";
    result += "SwapTotal: " + info.SwapTotal + "\n";
    result += "SwapFree: " + info.SwapFree + "\n";
    result += "Shmem: " + info.Shmem + "\n";
    result += "Slab: " + info.Slab + "\n";

    return result;
}

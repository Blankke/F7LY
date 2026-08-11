#pragma once

#include "types.hh"

#ifndef F7LY_PERF_DIAG
#define F7LY_PERF_DIAG 0
#endif

namespace perfdiag
{
    enum class Counter : uint16
    {
        UserTrap,
        Syscall,
        SyscallCycles,
        PageFault,
        PageFaultCycles,
        PageTableWalk,
        ProcessExit,
        ProcessExitCycles,
        VmunmapCall,
        VmunmapPages,
        VmunmapSparsePages,
        TeardownUnmapPages,
        TlbFlush,
        TlbFullFlush,
        TlbRemoteCpu,
        PmmAllocPage,
        PmmFreePage,        // 真正归还给 buddy 的物理页数
        PmmReleaseRef,      // 单页/连续/批量路径释放的 owner 引用数
        PmmBatchReleaseRef, // 其中经 release_pages_batch 释放的引用数
        TrapframeMapCheck,
        TrapframeRemap,
        FileFault,
        FileFaultReadBytes,
        FileCacheHit,
        FileCacheMiss,
        FileCacheEvict,
        FileCacheReadaheadPages,
        SysIoPoolHit,
        SysIoPoolMiss,
        SysIoTempAlloc,
        SysIoTempBytes,
        Ext4ReadBytes,
        Ext4WriteBytes,
        Ext4LockWaitCycles,
        BlockRequest,
        BlockRequestBytes,
        BlockWaitCycles,
        BlockMaxInflight,
        SchedulerIdle,
        SchedulerSwitch,
        Count,
    };

#if F7LY_PERF_DIAG
    uint64 timestamp();
    void add(Counter counter, uint64 value = 1);
    void set_max(Counter counter, uint64 value);
    void record_syscall(uint64 number, uint64 elapsed_cycles);
    uint64 count_set_bits(uint64 value);
    void reset();
    uint64 counter_sum(Counter counter);
    uint64 counter_max(Counter counter);
    uint64 syscall_count_sum(uint64 number);
    uint64 syscall_cycles_sum(uint64 number);
#else
    inline uint64 timestamp() { return 0; }
    inline void add(Counter, uint64 = 1) {}
    inline void set_max(Counter, uint64) {}
    inline void record_syscall(uint64, uint64) {}
    inline void reset() {}
#endif

    /**
     * 诊断构建中的轻量作用域计时器。正式构建会被编译为空对象。
     */
    class CycleScope
    {
    public:
#if F7LY_PERF_DIAG
        explicit CycleScope(Counter elapsed_counter)
            : counter_(elapsed_counter), begin_(timestamp()) {}
        ~CycleScope()
        {
            const uint64 end = timestamp();
            add(counter_, end >= begin_ ? end - begin_ : 0);
        }

    private:
        Counter counter_;
        uint64 begin_;
#else
        explicit CycleScope(Counter) {}
#endif
    };
}

#if F7LY_PERF_DIAG
#define F7LY_PERF_ADD(counter, value) \
    ::perfdiag::add(::perfdiag::Counter::counter, static_cast<uint64>(value))
#define F7LY_PERF_MAX(counter, value) \
    ::perfdiag::set_max(::perfdiag::Counter::counter, static_cast<uint64>(value))
#else
#define F7LY_PERF_ADD(counter, value) ((void)0)
#define F7LY_PERF_MAX(counter, value) ((void)0)
#endif

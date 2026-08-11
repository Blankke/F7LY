#pragma once

#include "types.hh"

#ifndef F7LY_PERF_DIAG
#define F7LY_PERF_DIAG 0
#endif

/*
 * 指标的唯一权威表。新增指标时只改这里；枚举、描述符和 /proc 导出均由它生成。
 * 参数：枚举名、稳定 ABI 名、类型、单位、聚合方式、说明。
 */
#define F7LY_PERF_METRIC_TABLE(X)                                                                  \
    X(UserTrap, "trap.user", Counter, "events", Sum, "用户态进入内核的 trap 次数")              \
    X(Syscall, "syscall.total", Counter, "calls", Sum, "系统调用总次数")                         \
    X(SyscallTimeTicks, "syscall.time_ticks", Duration, "time_ticks", Sum, "系统调用累计耗时")   \
    X(PageFault, "fault.total", Counter, "faults", Sum, "缺页处理次数")                           \
    X(PageFaultTimeTicks, "fault.time_ticks", Duration, "time_ticks", Sum, "缺页累计耗时")       \
    X(PageTableWalk, "pagetable.walk", Counter, "walks", Sum, "页表遍历次数")                     \
    X(ProcessExit, "process.exit", Counter, "exits", Sum, "进程退出次数")                         \
    X(ProcessExitTimeTicks, "process.exit_time_ticks", Duration, "time_ticks", Sum, "退出清理耗时") \
    X(VmunmapCall, "vmunmap.calls", Counter, "calls", Sum, "vmunmap 调用次数")                    \
    X(VmunmapPages, "vmunmap.pages", Counter, "pages", Sum, "vmunmap 页数")                       \
    X(VmunmapSparsePages, "vmunmap.sparse_pages", Counter, "pages", Sum, "稀疏解除映射页数")     \
    X(TeardownUnmapPages, "vmunmap.teardown_pages", Counter, "pages", Sum, "退出阶段解除映射页数") \
    X(TlbFlush, "tlb.flush", Counter, "flushes", Sum, "TLB 刷新次数")                             \
    X(TlbFullFlush, "tlb.full_flush", Counter, "flushes", Sum, "TLB 全量刷新次数")                \
    X(TlbRemoteCpu, "tlb.remote_cpus", Counter, "cpus", Sum, "远端 TLB 刷新 CPU 数")              \
    X(PmmAllocPage, "pmm.alloc_pages", Counter, "pages", Sum, "分配物理页数")                     \
    X(PmmFreePage, "pmm.free_pages", Counter, "pages", Sum, "归还 buddy 的物理页数")             \
    X(PmmReleaseRef, "pmm.release_refs", Counter, "refs", Sum, "释放物理页 owner 引用数")         \
    X(PmmBatchReleaseRef, "pmm.batch_release_refs", Counter, "refs", Sum, "批量释放引用数")       \
    X(TrapframeMapCheck, "trapframe.map_checks", Counter, "checks", Sum, "trapframe 映射检查次数") \
    X(TrapframeRemap, "trapframe.remaps", Counter, "remaps", Sum, "trapframe 重映射次数")         \
    X(FileFault, "file_fault.total", Counter, "faults", Sum, "文件映射缺页次数")                  \
    X(FileFaultReadBytes, "file_fault.read_bytes", Counter, "bytes", Sum, "文件缺页读取字节数")   \
    X(FileCacheHit, "file_cache.hits", Counter, "hits", Sum, "文件页缓存命中")                    \
    X(FileCacheMiss, "file_cache.misses", Counter, "misses", Sum, "文件页缓存未命中")             \
    X(FileCacheEvict, "file_cache.evicts", Counter, "evictions", Sum, "文件页缓存驱逐")           \
    X(FileCacheReadaheadPages, "file_cache.readahead_pages", Counter, "pages", Sum, "预读页数")   \
    X(SysIoPoolHit, "sysio.pool_hits", Counter, "hits", Sum, "sysio 临时缓冲池命中")              \
    X(SysIoPoolMiss, "sysio.pool_misses", Counter, "misses", Sum, "sysio 临时缓冲池未命中")       \
    X(SysIoTempAlloc, "sysio.temp_allocs", Counter, "allocations", Sum, "sysio 临时分配次数")     \
    X(SysIoTempBytes, "sysio.temp_bytes", Counter, "bytes", Sum, "sysio 临时分配字节数")          \
    X(Ext4ReadBytes, "ext4.read_bytes", Counter, "bytes", Sum, "ext4 块读取字节数")               \
    X(Ext4WriteBytes, "ext4.write_bytes", Counter, "bytes", Sum, "ext4 块写入字节数")             \
    X(Ext4LockWaitTimeTicks, "ext4.lock_wait_time_ticks", Duration, "time_ticks", Sum, "ext4 锁等待耗时") \
    X(BlockRequest, "block.requests", Counter, "requests", Sum, "块请求数")                       \
    X(BlockRequestBytes, "block.bytes", Counter, "bytes", Sum, "块请求字节数")                    \
    X(BlockWaitTimeTicks, "block.wait_time_ticks", Duration, "time_ticks", Sum, "块请求等待耗时") \
    X(BlockMaxInflight, "block.max_inflight", Gauge, "requests", Max, "最大并发块请求数")         \
    X(SchedulerIdle, "scheduler.idle", Counter, "ticks", Sum, "调度器空闲 tick")                  \
    X(SchedulerSwitch, "scheduler.switches", Counter, "switches", Sum, "上下文切换次数")

namespace perfdiag
{
    constexpr uint64 k_syscall_slots = 2048;
    constexpr uint64 k_flat_capacity = 2048;
    constexpr uint64 k_callchain_capacity = 512;
    constexpr uint64 k_callchain_depth = 8;

    enum class MetricKind : uint8 { Counter, Duration, Gauge };
    enum class Aggregate : uint8 { Sum, Max };

    enum class Counter : uint16
    {
#define F7LY_PERF_ENUM(id, name, kind, unit, aggregate, description) id,
        F7LY_PERF_METRIC_TABLE(F7LY_PERF_ENUM)
#undef F7LY_PERF_ENUM
        Count,
    };

    struct MetricDescriptor
    {
        Counter counter;
        const char *name;
        MetricKind kind;
        const char *unit;
        Aggregate aggregate;
        const char *description;
    };

    enum class ProfileBackend : uint8 { Auto, Timer, Pmu };
    enum class ProfileEvent : uint8 { CpuCycles, Instructions };

    struct ProfileConfig
    {
        bool active;
        ProfileBackend requested_backend;
        ProfileBackend active_backend;
        ProfileEvent event;
        uint32 frequency;
        uint64 period;
        bool callchain;
    };

    struct FlatSample
    {
        uint64 pc;
        uint64 count;
    };

    struct CallchainSample
    {
        uint64 pcs[k_callchain_depth];
        uint64 count;
        uint8 depth;
    };

#if F7LY_PERF_DIAG
    uint64 timestamp();
    uint64 timebase_hz();
    void add(Counter counter, uint64 value = 1);
    void set_max(Counter counter, uint64 value);
    void record_syscall(uint64 number, uint64 elapsed_time_ticks);
    uint64 count_set_bits(uint64 value);

    uint64 metrics_epoch();
    uint64 profile_epoch();
    uint64 next_snapshot_id();
    void reset_metrics();
    void reset_profile();
    void reset_all();

    uint64 descriptor_count();
    const MetricDescriptor &descriptor(uint64 index);
    uint64 counter_cpu(Counter counter, uint64 cpu);
    uint64 syscall_count_cpu(uint64 number, uint64 cpu);
    uint64 syscall_time_ticks_cpu(uint64 number, uint64 cpu);

    ProfileConfig profile_config();
    bool timer_frequency_valid(uint32 frequency);
    int profile_start(ProfileBackend backend, ProfileEvent event, uint32 frequency,
                      uint64 period, bool callchain);
    void profile_stop();
    bool pmu_available(ProfileEvent event);
    const char *backend_name(ProfileBackend backend);
    const char *event_name(ProfileEvent event);

    void on_timer_interrupt(bool from_kernel, uint64 pc, uint64 frame_pointer);
    void on_pmu_interrupt(bool from_kernel, uint64 pc, uint64 frame_pointer);
    bool flat_sample(uint64 cpu, uint64 index, FlatSample &out);
    bool callchain_sample(uint64 cpu, uint64 index, CallchainSample &out);
    uint64 profile_samples_cpu(uint64 cpu);
    uint64 profile_dropped_full_cpu(uint64 cpu);
    uint64 profile_invalid_pc_cpu(uint64 cpu);
    uint64 profile_user_skipped_cpu(uint64 cpu);
    uint64 profile_unwind_failed_cpu(uint64 cpu);

    uint64 symbol_count();
    bool symbol_at(uint64 index, uint64 &start, uint64 &end, const char *&name);
#else
    inline uint64 timestamp() { return 0; }
    inline uint64 timebase_hz() { return 0; }
    inline void add(Counter, uint64 = 1) {}
    inline void set_max(Counter, uint64) {}
    inline void record_syscall(uint64, uint64) {}
    inline void reset_metrics() {}
    inline void reset_profile() {}
    inline void reset_all() {}
    inline void profile_stop() {}
    inline void on_timer_interrupt(bool, uint64, uint64) {}
    inline void on_pmu_interrupt(bool, uint64, uint64) {}
#endif

    class TimeScope
    {
    public:
#if F7LY_PERF_DIAG
        explicit TimeScope(Counter elapsed_counter) : counter_(elapsed_counter), begin_(timestamp()) {}
        ~TimeScope()
        {
            const uint64 end = timestamp();
            add(counter_, end >= begin_ ? end - begin_ : 0);
        }
    private:
        Counter counter_;
        uint64 begin_;
#else
        explicit TimeScope(Counter) {}
#endif
    };
}

#if F7LY_PERF_DIAG
#define F7LY_PERF_ADD(counter, value) \
    ::perfdiag::add(::perfdiag::Counter::counter, static_cast<uint64>(value))
#define F7LY_PERF_MAX(counter, value) \
    ::perfdiag::set_max(::perfdiag::Counter::counter, static_cast<uint64>(value))
#define F7LY_PERF_SCOPE(counter) \
    ::perfdiag::TimeScope f7ly_perf_scope_##__LINE__(::perfdiag::Counter::counter)
#else
#define F7LY_PERF_ADD(counter, value) ((void)0)
#define F7LY_PERF_MAX(counter, value) ((void)0)
#define F7LY_PERF_SCOPE(counter) ((void)0)
#endif

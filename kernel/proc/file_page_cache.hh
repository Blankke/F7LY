#pragma once

#include "types.hh"

namespace fs
{
    class file;
    struct FilePageCacheIdentity;
}

namespace proc::file_page_cache
{
    /** 在内核堆就绪后、任何文件访问前显式构造全局缓存状态。 */
    void init();

    enum class AcquireStatus : uint8
    {
        Acquired = 0,
        Bypass,
        Retry,
        Error,
    };

    struct ContentState
    {
        uint64 sequence = 0;
        uint32 active_mutations = 0;
    };

    struct AcquireResult
    {
        AcquireStatus status = AcquireStatus::Bypass;
        uint64 page = 0;
        uint64 content_epoch = 0;
        int error = 0;
        bool hit = false;
    };

    /**
     * 获取一张完整的、只读 clean 文件页。
     *
     * 返回的 page 已为调用者额外 retain 一份 owner 引用；全局缓存自己保留
     * 独立 owner 引用。调用者若不接管该页，必须用 PMM free_page() 归还引用。
     */
    AcquireResult acquire_clean_page(fs::file *file,
                                     const fs::FilePageCacheIdentity &identity,
                                     uint64 page_index,
                                     uint64 file_offset);

    /** 当前文件内容 sequence；每个 mutation begin/end 都会推进。 */
    uint64 content_epoch(const fs::FilePageCacheIdentity &identity);

    /**
     * 取得 sequence + 正在修改的 writer 数。fault 只能在
     * active_mutations == 0 时读取/发布文件页。
     */
    ContentState content_state(const fs::FilePageCacheIdentity &identity);

    /** 复核快照仍是当前稳定内容代。 */
    bool content_state_matches(const fs::FilePageCacheIdentity &identity,
                               uint64 sequence);

    /**
     * 文件内容修改门禁。begin 会为尚未 mmap 的 inode 建立
     * 短命 state，以封住“首次 mmap 与 write 并发”窗口；end
     * 必须与每次成功 begin 成对。两端都会摘除目标页并
     * 推进 sequence；多 writer 通过 active_mutations 计数叠加。
     */
    bool begin_mutation(const fs::FilePageCacheIdentity &identity,
                        uint64 offset,
                        uint64 length);
    void end_mutation(const fs::FilePageCacheIdentity &identity,
                      uint64 offset,
                      uint64 length);

    /**
     * 失效与字节范围相交的缓存页并递增内容 epoch。
     * length == UINT64_MAX 表示整个 inode。
     *
     * 这是无 active writer 语义的一次性失效。真实修改内容的
     * 路径必须使用 begin_mutation()/end_mutation()，不能用两次
     * invalidate_range() 代替门禁。未登记的 inode 仍直接返回。
     */
    void invalidate_range(const fs::FilePageCacheIdentity &identity,
                          uint64 offset,
                          uint64 length);

    /**
     * 物理页分配失败时主动淘汰最多 max_pages 张 clean 文件缓存页。
     * 调用方不得持有文件页缓存内部锁；返回实际释放的 cache owner 数量。
     */
    uint32 reclaim_clean_pages(uint32 max_pages);

    /** 一次 I/O 预读连续 16 张完整页；失败仅放弃预读，不影响当前缺页。 */
    void readahead_16_pages(fs::file *file,
                            const fs::FilePageCacheIdentity &identity,
                            uint64 first_page_index,
                            uint64 first_file_offset);
}

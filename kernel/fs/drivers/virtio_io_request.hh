#pragma once

#include "types.hh"

struct buf;

namespace virtio_blk
{
    /**
     * @brief 请求完成后的唤醒方式。
     *
     * `BufferCache` 用于传统 `bread/bwrite` 路径，
     * `CallerWait` 用于 ext4 blockdev 等直接同步提交路径。
     */
    enum class IoCompletionType : uint8
    {
        BufferCache = 0,
        CallerWait = 1,
    };

    /**
     * @brief 统一的 virtio 块层请求对象。
     *
     * 设计上将“传输参数”“优先级调度元数据”“完成同步状态”统一封装，
     * 避免把调度细节直接污染到 `struct buf` 或架构驱动实现中。
     */
    struct IoRequest
    {
        void *data;
        uint32 data_len;
        uint64 start_sector;
        bool write;

        uint64 request_bytes;
        uint submit_pid;
        int submit_nice;

        void *wait_channel;
        IoCompletionType completion_type;
        struct buf *owner_buf;
        volatile bool completed;
        int io_status;

        int service_class;
        IoRequest *flow_next;
    };
} // namespace virtio_blk

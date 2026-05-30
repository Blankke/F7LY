#pragma once

#include "types.hh"
#include "spinlock.hh"
#include "mem/memlayout.hh"
#include "fs/buf.hh"
#include "virtio_blk.hh"
#include "virtio_blk_transport.hh"
#include "virtio_io_request.hh"
#include "virtio_priority_borrow_scheduler.hh"

namespace virtio_blk
{
    /**
     * @brief 跨架构复用的 virtio 块队列封装。
     *
     * 该类只负责：
     * 1. virtqueue 描述符管理；
     * 2. 优先级借用调度与 in-flight 请求跟踪；
     * 3. 睡眠/轮询等待与完成回收。
     *
     * 设备发现、feature 协商、queue addr 注册与中断控制由架构适配层完成。
     */
    class VirtioBlkQueue
    {
    public:
        struct InitArgs
        {
            const char *lock_name;
            int owner_token;
            VirtioBlkTransport *transport;
        };

        void initialize(const InitArgs &args);

        void *pages_base();
        uint64 pages_dma_base() const;
        VRingDesc *desc_area();
        uint16 *avail_area();
        UsedArea *used_area();

        void submit_and_wait(struct buf *b, int write);
        int submit_transfer_and_wait(void *data, uint32 data_len, uint64 start_sector, bool write);
        void handle_interrupt();
        void poll_once();

    private:
        struct RequestInfo
        {
            IoRequest *request;
            VirtioBlkReq header;
            char status;
            uint64 dispatch_us;
        };

        struct ClassBandwidthStats
        {
            uint64 completed_bytes;
            uint64 completed_requests;
            uint64 window_start_us;
            uint64 ewma_bps;
        };

        struct PriorityBorrowTraceStats
        {
            uint64 contended_dispatches;
            uint64 high_wins;
            uint64 low_while_high_pending;
            uint64 selected_by_class[PriorityBorrowScheduler::k_class_count];
            uint64 last_report_contended;
        };

        struct DescChain
        {
            int head;
            int middle;
            int tail;
        };

        static uint64 now_us();
        static uint64 ceil_div_u64(uint64 numerator, uint64 denominator);
        static int first_pending_class(uint32 pending_mask);
        static bool has_lower_pending_class(uint32 pending_mask, int service_class);

        void reset_runtime();
        void reset_bandwidth_stats_locked();
        void reset_priority_trace_locked();
        void record_priority_trace_locked(uint32 pending_mask, const IoRequest *request);
        void record_completion_stats_locked(const IoRequest *request, uint64 dispatch_us, uint64 finish_us);
        bool has_free_desc_chain_locked() const;
        bool alloc_desc_chain_locked(DescChain &chain);
        void free_desc_locked(int idx);
        void free_chain_locked(int head_idx);
        bool submit_one_locked(IoRequest *request);
        bool dispatch_pending_locked();
        void process_used_locked();
        void submit_request_and_wait(IoRequest &request);

        alignas(PGSIZE) char pages_[2 * PGSIZE];
        VRingDesc *desc_;
        uint16 *avail_;
        UsedArea *used_;
        char free_[k_queue_size];
        uint16 used_idx_;
        int owner_token_;
        int inflight_count_;
        ClassBandwidthStats class_stats_[PriorityBorrowScheduler::k_class_count];
        PriorityBorrowTraceStats priority_trace_;
        RequestInfo info_[k_queue_size];
        SpinLock lock_;
        VirtioBlkTransport *transport_;
        PriorityBorrowScheduler scheduler_;
    };
} // namespace virtio_blk

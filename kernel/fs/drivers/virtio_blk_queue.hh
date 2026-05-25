#pragma once

#include "types.hh"
#include "spinlock.hh"
#include "mem/memlayout.hh"
#include "fs/buf.hh"
#include "virtio_blk.hh"
#include "virtio_blk_transport.hh"
#include "virtio_io_request.hh"
#include "virtio_mclock_scheduler.hh"

namespace virtio_blk
{
    /**
     * @brief 跨架构复用的 virtio 块队列封装。
     *
     * 该类只负责：
     * 1. virtqueue 描述符管理；
     * 2. mClock 调度与 in-flight 请求跟踪；
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

        struct DescChain
        {
            int head;
            int middle;
            int tail;
        };

        static uint64 now_us();
        static uint64 ceil_div_u64(uint64 numerator, uint64 denominator);

        void reset_runtime();
        bool has_free_desc_chain_locked() const;
        bool alloc_desc_chain_locked(DescChain &chain);
        void free_desc_locked(int idx);
        void free_chain_locked(int head_idx);
        bool submit_one_locked(IoRequest *request);
        bool dispatch_pending_locked(uint64 *next_gate_us = nullptr);
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
        uint64 ewma_bps_;
        RequestInfo info_[k_queue_size];
        SpinLock lock_;
        VirtioBlkTransport *transport_;
        MClockScheduler scheduler_;
    };
} // namespace virtio_blk

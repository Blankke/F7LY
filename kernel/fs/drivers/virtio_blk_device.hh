#pragma once

#include "fs/buf.hh"
#include "virtio_blk_queue.hh"
#include "virtio_blk_transport.hh"

namespace virtio_blk
{
    /**
     * @brief 跨架构复用的 virtio 块设备对象。
     *
     * 该类负责聚合：
     * 1. 传输层对象；
     * 2. 队列对象；
     * 3. 面向块层调用方的同步提交、中断处理与 DMA 页信息查询接口。
     *
     * 这样架构适配文件只需要完成 feature 协商、queue 地址注册和中断入口转发，
     * 不再直接操心调度器和请求等待细节。
     */
    class VirtioBlkDevice
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
        VirtioBlkTransport *transport_ = nullptr;
        VirtioBlkQueue queue_;
    };
} // namespace virtio_blk

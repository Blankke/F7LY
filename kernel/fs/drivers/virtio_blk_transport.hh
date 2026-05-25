#pragma once

#include "types.hh"

namespace virtio_blk
{
    /**
     * @brief virtio 块设备传输层抽象。
     *
     * 该接口把“DMA 地址翻译、队列通知、used ring 可见性处理、中断确认、
     * 以及等待模式选择”等传输相关细节统一收敛起来，让上层块设备与队列逻辑
     * 不再感知 MMIO / PCI 这类架构差异。
     */
    class VirtioBlkTransport
    {
    public:
        virtual ~VirtioBlkTransport() = default;

        virtual uint64 dma_addr(const void *ptr) const = 0;
        virtual void notify_queue(uint16 queue_index) = 0;
        virtual void prepare_used_check() = 0;
        virtual void ack_interrupt() = 0;
        virtual bool polling_wait() const = 0;
    };
} // namespace virtio_blk

#pragma once

#include "types.hh"

namespace net::virtio_transport
{
// 传输层只接收队列内存，不持有公共网卡状态。这样 MMIO/PCI 的寄存器与
// 板级资源不会反向渗入 VirtIO net 的收发、descriptor 和 ring 逻辑。
struct QueueMemory
{
    void *descriptor;
    void *available;
    void *used;
    uint64 descriptor_dma;
    uint64 available_dma;
    uint64 used_dma;
};

struct PrepareArgs
{
    QueueMemory receive_queue;
    QueueMemory transmit_queue;
    uint16 queue_size;
    uint64 driver_features;
    uint8 *mac_address;
    uint32 mac_address_length;
};

inline constexpr uint16 k_receive_queue_index = 0;
inline constexpr uint16 k_transmit_queue_index = 1;
inline constexpr uint32 k_feature_mac = 5;

// 每个 QEMU 画像必须在 mk/platform 中恰好选择一份实现。prepare() 完成
// 设备发现、feature 协商和队列配置，但暂不置 DRIVER_OK，便于公共层先登记 IRQ。
bool prepare(const PrepareArgs &args, uint64 *negotiated_features);
void activate();
void notify_queue(uint16 queue_index);
void acknowledge_interrupt();
uint32 interrupt_source();
} // namespace net::virtio_transport

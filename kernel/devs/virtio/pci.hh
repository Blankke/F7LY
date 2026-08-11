#pragma once

#include "platform/pci.hh"
#include "types.hh"

// VirtIO PCI capability 类型来自 VirtIO 1.x 规范。它们属于设备传输协议，
// 不属于文件系统、网络栈、trap 或 LoongArch 中断实现。
inline constexpr uint8 k_virtio_pci_cap_common_cfg = 1;
inline constexpr uint8 k_virtio_pci_cap_notify_cfg = 2;
inline constexpr uint8 k_virtio_pci_cap_isr_cfg = 3;
inline constexpr uint8 k_virtio_pci_cap_device_cfg = 4;
inline constexpr uint8 k_pci_vendor_capability_id = 0x09;
inline constexpr uint32 k_virtio_feature_version_1 = 32;

struct VirtioPciCapability
{
    uint8 capability_id;
    uint8 next;
    uint8 length;
    uint8 type;
    uint8 bar;
    uint8 id;
    uint8 padding[2];
    uint32 offset;
    uint32 region_length;
};
static_assert(sizeof(VirtioPciCapability) == 16);

struct VirtioPciCommonConfig
{
    uint32 device_feature_select;
    uint32 device_feature;
    uint32 driver_feature_select;
    uint32 driver_feature;
    uint16 config_msix_vector;
    uint16 num_queues;
    uint8 device_status;
    uint8 config_generation;

    uint16 queue_select;
    uint16 queue_size;
    uint16 queue_msix_vector;
    uint16 queue_enable;
    uint16 queue_notify_offset;
    // VirtIO PCI 把 64 位队列地址定义为低 32 位、随后高 32 位两个字段。
    // 不能写成 uint64：common_cfg 只保证 4 字节对齐，直接 64 位 MMIO
    // 访问既违反传输规范，也会在合法的 4(mod 8) 地址上产生未对齐访问。
    uint32 queue_desc_low;
    uint32 queue_desc_high;
    uint32 queue_driver_low;
    uint32 queue_driver_high;
    uint32 queue_device_low;
    uint32 queue_device_high;
    uint16 queue_notify_data;
    uint16 queue_reset;
};
static_assert(__builtin_offsetof(VirtioPciCommonConfig, queue_select) == 22);
static_assert(__builtin_offsetof(VirtioPciCommonConfig, device_status) == 20);
static_assert(__builtin_offsetof(VirtioPciCommonConfig, queue_notify_offset) == 30);
static_assert(__builtin_offsetof(VirtioPciCommonConfig, queue_desc_low) == 32);
static_assert(__builtin_offsetof(VirtioPciCommonConfig, queue_device_low) == 48);
static_assert(alignof(VirtioPciCommonConfig) == alignof(uint32));
inline constexpr uint32 k_virtio_pci_common_cfg_min_length =
    __builtin_offsetof(VirtioPciCommonConfig, queue_device_high) +
    sizeof(uint32);
static_assert(k_virtio_pci_common_cfg_min_length == 56);

struct virtio_pci_hw_t
{
    uint32 notify_offset_multiplier;
    uint32 notify_cfg_length;
    uint32 device_cfg_length;
    void *common_cfg;
    void *isr_cfg;
    void *device_cfg;
    void *notify_cfg;
};

int virtio_pci_read_caps(virtio_pci_hw_t *hardware,
                         platform::pci::FunctionAddress address);
// 把设备复位到 status=0，并在有界时间内等待设备确认。调用方不能在
// 复位仍进行时继续写 ACKNOWLEDGE/DRIVER 状态。
bool virtio_pci_reset(virtio_pci_hw_t *hardware);
// 设备专属配置必须在 config_generation 前后一致时才算读取成功；bytes
// 用于 8 位字段，u64 按 PCI 规则拆成两个 32 位访问。
bool virtio_pci_read_device_config_bytes(virtio_pci_hw_t *hardware,
                                         uint32 offset, void *buffer,
                                         uint32 length);
bool virtio_pci_read_device_config_u64(virtio_pci_hw_t *hardware,
                                       uint32 offset, uint64 &value);
uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hardware);
void virtio_pci_set_driver_features(virtio_pci_hw_t *hardware, uint64 features);
uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hardware, uint16 queue_index);
void virtio_pci_set_queue_size(virtio_pci_hw_t *hardware, uint16 queue_index,
                               uint16 queue_size);
void virtio_pci_set_queue_addresses(virtio_pci_hw_t *hardware,
                                    uint16 queue_index,
                                    uint64 descriptor_dma,
                                    uint64 available_dma,
                                    uint64 used_dma);
void virtio_pci_set_queue_notify(virtio_pci_hw_t *hardware,
                                 uint16 queue_index);
void virtio_pci_set_queue_enable(virtio_pci_hw_t *hardware,
                                 uint16 queue_index);
uint16 virtio_pci_get_queue_enable(virtio_pci_hw_t *hardware,
                                   uint16 queue_index);
uint8 virtio_pci_clear_isr(virtio_pci_hw_t *hardware);
uint8 virtio_pci_get_status(virtio_pci_hw_t *hardware);
void virtio_pci_set_status(virtio_pci_hw_t *hardware, uint8 status);

#include "devs/virtio/pci.hh"

#include "hal/arch.hh"
#include "printer.hh"
#include "tm/time.hh"

namespace
{
constexpr uint16 k_capability_pointer_register = 0x34;
constexpr uint16 k_legacy_config_header_size = 0x100;
constexpr uint16 k_first_valid_capability_offset = 0x40;
constexpr uint32 k_max_capability_count = 48;
constexpr uint64 k_device_reset_timeout_us = 1'000'000ULL;
constexpr uint32 k_max_device_config_read_attempts = 32;

struct PciCapabilityHeader
{
    uint8 capability_id;
    uint8 next;
    uint8 length;
    uint8 type;
};
static_assert(sizeof(PciCapabilityHeader) == 4);

bool valid_notify_multiplier(uint32 multiplier)
{
    // 未协商 VIRTIO_F_NOTIFICATION_DATA 时，规范允许 0 或不小于 2 的
    // 2 的幂。0 表示所有 virtqueue 共用 capability 起始地址，并非错误。
    return multiplier == 0 ||
           (multiplier >= 2 && (multiplier & (multiplier - 1)) == 0);
}

volatile VirtioPciCommonConfig *common_config(virtio_pci_hw_t *hardware)
{
    return reinterpret_cast<volatile VirtioPciCommonConfig *>(
        hardware->common_cfg);
}

void *capability_region(platform::pci::FunctionAddress address,
                        const VirtioPciCapability &capability)
{
    platform::pci::MemoryBar bar{};
    if (!platform::pci::memory_bar(address, capability.bar, bar) ||
        capability.region_length == 0)
    {
        return nullptr;
    }
    // capability 描述的是某一个 BAR 内的子区间。只检查整机 MMIO 窗口
    // 会让错误 offset 越过本 BAR 后落进另一个设备的寄存器区。
    if (capability.offset >= bar.size ||
        capability.region_length > bar.size - capability.offset ||
        capability.offset > ~uint64{0} - bar.bus_address)
    {
        return nullptr;
    }

    return reinterpret_cast<void *>(platform::pci::mapped_mmio_address(
        bar.bus_address + capability.offset, capability.region_length));
}

uint16 queue_notify_offset(virtio_pci_hw_t *hardware, uint16 queue_index)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();
    return config->queue_notify_offset;
}

void write_dma_address(volatile uint32 &low, volatile uint32 &high,
                       uint64 address)
{
    // 规范要求 64 位字段按低 32 位、随后高 32 位分别访问。
    low = static_cast<uint32>(address);
    high = static_cast<uint32>(address >> 32);
}
} // namespace

int virtio_pci_read_caps(virtio_pci_hw_t *hardware,
                         platform::pci::FunctionAddress address)
{
    if (hardware == nullptr)
    {
        return -1;
    }

    hardware->notify_offset_multiplier = 0;
    hardware->notify_cfg_length = 0;
    hardware->device_cfg_length = 0;
    hardware->common_cfg = nullptr;
    hardware->isr_cfg = nullptr;
    hardware->device_cfg = nullptr;
    hardware->notify_cfg = nullptr;

    uint8 position = platform::pci::config_read8(
        address, k_capability_pointer_register);
    uint32 capability_count = 0;
    while (position != 0)
    {
        // PCI capability 必须落在 256 字节配置头内并按 4 字节对齐。数量
        // 上限同时阻止损坏的 next 指针形成死循环。
        if (position < k_first_valid_capability_offset ||
            (position & 3U) != 0 ||
            position > k_legacy_config_header_size -
                           sizeof(PciCapabilityHeader) ||
            capability_count++ >= k_max_capability_count)
        {
            return -1;
        }

        PciCapabilityHeader header{};
        platform::pci::config_read(address, position, &header,
                                   sizeof(header));
        const uint8 next = header.next;
        if (header.capability_id != k_pci_vendor_capability_id)
        {
            position = next;
            continue;
        }
        if (header.length < sizeof(PciCapabilityHeader) ||
            header.length > k_legacy_config_header_size - position)
        {
            return -1;
        }

        // 其它厂商 capability 或未来的 VirtIO cfg_type 由对应驱动解释。
        // 这里只读取本传输层认识、且长度足以覆盖公共头的结构。
        if (header.type < k_virtio_pci_cap_common_cfg ||
            header.type > k_virtio_pci_cap_device_cfg)
        {
            position = next;
            continue;
        }
        if (header.length < sizeof(VirtioPciCapability) ||
            position > k_legacy_config_header_size -
                           sizeof(VirtioPciCapability))
        {
            return -1;
        }

        VirtioPciCapability capability{};
        platform::pci::config_read(address, position, &capability,
                                   sizeof(capability));
        switch (capability.type)
        {
        case k_virtio_pci_cap_common_cfg:
            // 规范允许同类型 capability 提供不同访问方式；使用第一个
            // 当前平台能安全映射的实例，后续同类型实例不再覆盖它。
            if (hardware->common_cfg != nullptr)
            {
                break;
            }
            // 本驱动会一直访问 queue_device，因此 common_cfg 至少要覆盖
            // 该 64 位字段末尾，且首个 32 位字段必须自然对齐。
            if ((capability.offset & (alignof(uint32) - 1)) != 0 ||
                capability.region_length <
                    k_virtio_pci_common_cfg_min_length)
            {
                break;
            }
            hardware->common_cfg = capability_region(address, capability);
            break;
        case k_virtio_pci_cap_notify_cfg:
        {
            if (hardware->notify_cfg != nullptr)
            {
                break;
            }
            if (capability.length < sizeof(VirtioPciCapability) +
                                        sizeof(uint32) ||
                (capability.offset & (alignof(uint16) - 1)) != 0 ||
                capability.region_length < sizeof(uint16))
            {
                break;
            }
            const uint32 multiplier = platform::pci::config_read32(
                address, position + sizeof(VirtioPciCapability));
            if (!valid_notify_multiplier(multiplier))
            {
                break;
            }
            void *region = capability_region(address, capability);
            if (region == nullptr)
            {
                break;
            }
            hardware->notify_offset_multiplier = multiplier;
            hardware->notify_cfg_length = capability.region_length;
            hardware->notify_cfg = region;
            break;
        }
        case k_virtio_pci_cap_device_cfg:
            if (hardware->device_cfg != nullptr ||
                (capability.offset & (alignof(uint32) - 1)) != 0)
            {
                break;
            }
            hardware->device_cfg = capability_region(address, capability);
            if (hardware->device_cfg != nullptr)
            {
                hardware->device_cfg_length = capability.region_length;
            }
            break;
        case k_virtio_pci_cap_isr_cfg:
            if (hardware->isr_cfg != nullptr ||
                capability.region_length < sizeof(uint8))
            {
                break;
            }
            // ISR 状态按协议读取 1 字节，无额外对齐要求。
            hardware->isr_cfg = capability_region(address, capability);
            break;
        default:
            break;
        }
        position = next;
    }

    return hardware->common_cfg != nullptr &&
                   hardware->notify_cfg != nullptr &&
                   hardware->isr_cfg != nullptr
               ? 0
               : -1;
}

bool virtio_pci_reset(virtio_pci_hw_t *hardware)
{
    if (hardware == nullptr || hardware->common_cfg == nullptr)
    {
        return false;
    }

    virtio_pci_set_status(hardware, 0);
    const uint64 start = platform::clock_backend::read_ticks();
    const uint64 timeout =
        tmm::microseconds_to_cycles(k_device_reset_timeout_us);
    while (virtio_pci_get_status(hardware) != 0)
    {
        if (platform::clock_backend::read_ticks() - start >= timeout)
        {
            return false;
        }
        asm volatile("" ::: "memory");
    }
    return true;
}

bool virtio_pci_read_device_config_bytes(virtio_pci_hw_t *hardware,
                                         uint32 offset, void *buffer,
                                         uint32 length)
{
    if (hardware == nullptr || hardware->common_cfg == nullptr ||
        hardware->device_cfg == nullptr || buffer == nullptr || length == 0 ||
        offset > hardware->device_cfg_length ||
        length > hardware->device_cfg_length - offset)
    {
        return false;
    }

    auto *destination = static_cast<uint8 *>(buffer);
    auto *source = reinterpret_cast<volatile uint8 *>(
        reinterpret_cast<uint64>(hardware->device_cfg) + offset);
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    for (uint32 attempt = 0; attempt < k_max_device_config_read_attempts;
         ++attempt)
    {
        const uint8 generation = config->config_generation;
        dsb();
        for (uint32 index = 0; index < length; ++index)
        {
            destination[index] = source[index];
        }
        dsb();
        if (generation == config->config_generation)
        {
            return true;
        }
    }
    return false;
}

bool virtio_pci_read_device_config_u64(virtio_pci_hw_t *hardware,
                                       uint32 offset, uint64 &value)
{
    if (hardware == nullptr || hardware->common_cfg == nullptr ||
        hardware->device_cfg == nullptr ||
        (offset & (alignof(uint32) - 1)) != 0 ||
        offset > hardware->device_cfg_length ||
        sizeof(uint64) > hardware->device_cfg_length - offset)
    {
        return false;
    }

    auto *source = reinterpret_cast<volatile uint32 *>(
        reinterpret_cast<uint64>(hardware->device_cfg) + offset);
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    for (uint32 attempt = 0; attempt < k_max_device_config_read_attempts;
         ++attempt)
    {
        const uint8 generation = config->config_generation;
        dsb();
        const uint32 low = source[0];
        const uint32 high = source[1];
        dsb();
        if (generation == config->config_generation)
        {
            value = static_cast<uint64>(low) |
                    (static_cast<uint64>(high) << 32);
            return true;
        }
    }
    return false;
}

uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hardware)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->device_feature_select = 0;
    dsb();
    const uint64 low = config->device_feature;

    config->device_feature_select = 1;
    dsb();
    const uint64 high = config->device_feature;
    return (high << 32) | low;
}

void virtio_pci_set_driver_features(virtio_pci_hw_t *hardware, uint64 features)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->driver_feature_select = 0;
    dsb();
    config->driver_feature = static_cast<uint32>(features);

    config->driver_feature_select = 1;
    dsb();
    config->driver_feature = static_cast<uint32>(features >> 32);
}

uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hardware,
                                 uint16 queue_index)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();
    return config->queue_size;
}

void virtio_pci_set_queue_size(virtio_pci_hw_t *hardware,
                               uint16 queue_index, uint16 queue_size)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();
    config->queue_size = queue_size;
}

void virtio_pci_set_queue_addresses(virtio_pci_hw_t *hardware,
                                    uint16 queue_index,
                                    uint64 descriptor_dma,
                                    uint64 available_dma,
                                    uint64 used_dma)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();

    // 传输层只写调用者已完成页表翻译的 DMA 地址，不能把 CPU 虚拟指针
    // 当成设备地址。这样 DMW 与普通内核虚拟内存都使用同一协议入口。
    write_dma_address(config->queue_desc_low, config->queue_desc_high,
                      descriptor_dma);
    write_dma_address(config->queue_driver_low, config->queue_driver_high,
                      available_dma);
    write_dma_address(config->queue_device_low, config->queue_device_high,
                      used_dma);
}

void virtio_pci_set_queue_notify(virtio_pci_hw_t *hardware,
                                 uint16 queue_index)
{
    const uint64 notify_offset = queue_notify_offset(hardware, queue_index);
    uint64 byte_offset = 0;
    if (hardware->notify_offset_multiplier != 0)
    {
        if (notify_offset >
            ~uint64{0} / hardware->notify_offset_multiplier)
        {
            panic("virtio pci: notify offset overflow");
        }
        byte_offset =
            static_cast<uint64>(notify_offset) *
            hardware->notify_offset_multiplier;
    }
    if (hardware->notify_cfg_length < sizeof(uint16) ||
        byte_offset > hardware->notify_cfg_length - sizeof(uint16))
    {
        panic("virtio pci: queue notify outside capability: queue=%u offset=%p",
              queue_index, byte_offset);
    }
    const uint64 notify_base =
        reinterpret_cast<uint64>(hardware->notify_cfg);
    if (notify_base > ~uint64{0} - byte_offset)
    {
        panic("virtio pci: notify address overflow");
    }
    const uint64 notify_address = notify_base + byte_offset;
    if ((notify_address & (alignof(uint16) - 1)) != 0)
    {
        panic("virtio pci: unaligned notify address: queue=%u address=%p",
              queue_index, notify_address);
    }
    auto *notify = reinterpret_cast<volatile uint16 *>(notify_address);

    // 未协商 VIRTIO_F_NOTIFICATION_DATA 时，notify 内容是 queue index。
    // 旧实现固定写 1，导致 RX queue 0 实际通知了 TX queue 1。
    *notify = queue_index;
}

void virtio_pci_set_queue_enable(virtio_pci_hw_t *hardware,
                                 uint16 queue_index)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();
    config->queue_enable = 1;
}

uint16 virtio_pci_get_queue_enable(virtio_pci_hw_t *hardware,
                                   uint16 queue_index)
{
    volatile VirtioPciCommonConfig *config = common_config(hardware);
    config->queue_select = queue_index;
    dsb();
    return config->queue_enable;
}

uint8 virtio_pci_clear_isr(virtio_pci_hw_t *hardware)
{
    // VirtIO PCI ISR status 只有 1 字节，读取即确认。读取 32 位会越过该
    // capability 的协议宽度，在更严格的设备模型上可能产生副作用。
    return *reinterpret_cast<volatile uint8 *>(hardware->isr_cfg);
}

uint8 virtio_pci_get_status(virtio_pci_hw_t *hardware)
{
    return common_config(hardware)->device_status;
}

void virtio_pci_set_status(virtio_pci_hw_t *hardware, uint8 status)
{
    common_config(hardware)->device_status = status;
    dsb();
}

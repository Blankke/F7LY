#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <memory>
#include "types.hh"
#include "spinlock.hh"
#include "virtio.hh" // 包含 VirtIO 基本定义

#pragma once

#include <cstdint>
#include "volatile.hh" // 用于 volatile 访问的包装器

// 设备状态标志
enum class DeviceStatus : uint8_t
{
    NONE = 0,
    ACKNOWLEDGE = 1,
    DRIVER = 2,
    DRIVER_OK = 4,
    FEATURES_OK = 8,
    DEVICE_NEEDS_RESET = 64,
    FAILED = 128
};

inline DeviceStatus operator|(DeviceStatus a, DeviceStatus b)
{
    return static_cast<DeviceStatus>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline DeviceStatus operator&(DeviceStatus a, DeviceStatus b)
{
    return static_cast<DeviceStatus>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

// VirtIO 设备类型
enum class DeviceType : uint32_t
{
    NETWORK = 1,
    BLOCK = 2,
    // 其他设备类型...
};

// VirtIO MMIO 设备头结构
struct VirtIOHeader
{
    // Magic 值 ("virt" 字符串)
    ReadOnly<uint32_t> magic;

    // 设备版本号 (legacy 设备返回 0x1)
    ReadOnly<uint32_t> version;

    // Virtio 子系统设备 ID
    ReadOnly<uint32_t> device_id;

    // Virtio 子系统厂商 ID
    ReadOnly<uint32_t> vendor_id;

    // 设备支持的特性标志
    ReadOnly<uint32_t> device_features;

    // 设备特性字选择
    WriteOnly<uint32_t> device_features_sel;

    // 保留区域
    ReadOnly<uint32_t> __r1[2];

    // 驱动激活的特性标志
    WriteOnly<uint32_t> driver_features;

    // 驱动特性字选择
    WriteOnly<uint32_t> driver_features_sel;

    // 客户机页面大小 (legacy)
    WriteOnly<uint32_t> legacy_guest_page_size;

    // 保留区域
    ReadOnly<uint32_t> __r2;

    // 虚拟队列索引选择
    WriteOnly<uint32_t> queue_sel;

    // 最大虚拟队列大小
    ReadOnly<uint32_t> queue_num_max;

    // 虚拟队列大小设置
    WriteOnly<uint32_t> queue_num;

    // Used Ring 对齐 (legacy)
    WriteOnly<uint32_t> legacy_queue_align;

    // 虚拟队列物理页号 (legacy)
    Volatile<uint32_t> legacy_queue_pfn;

    // 队列就绪标志 (modern)
    Volatile<uint32_t> queue_ready;

    // 保留区域
    ReadOnly<uint32_t> __r3[2];

    // 队列通知
    WriteOnly<uint32_t> queue_notify;

    // 保留区域
    ReadOnly<uint32_t> __r4[3];

    // 中断状态
    ReadOnly<uint32_t> interrupt_status;

    // 中断确认
    WriteOnly<uint32_t> interrupt_ack;

    // 保留区域
    ReadOnly<uint32_t> __r5[2];

    // 设备状态
    Volatile<DeviceStatus> status;

    // 保留区域
    ReadOnly<uint32_t> __r6[3];

    // 队列描述符表低32位 (modern)
    WriteOnly<uint32_t> queue_desc_low;

    // 队列描述符表高32位 (modern)
    WriteOnly<uint32_t> queue_desc_high;

    // 保留区域
    ReadOnly<uint32_t> __r7[2];

    // 驱动区域低32位 (modern)
    WriteOnly<uint32_t> queue_driver_low;

    // 驱动区域高32位 (modern)
    WriteOnly<uint32_t> queue_driver_high;

    // 保留区域
    ReadOnly<uint32_t> __r8[2];

    // 设备区域低32位 (modern)
    WriteOnly<uint32_t> queue_device_low;

    // 设备区域高32位 (modern)
    WriteOnly<uint32_t> queue_device_high;

    // 保留区域
    ReadOnly<uint32_t> __r9[21];

    // 配置空间生成号
    ReadOnly<uint32_t> config_generation;

// 构造一个测试用的虚拟头
#ifdef TESTING
    static VirtIOHeader make_fake_header(
        uint32_t version,
        uint32_t device_id,
        uint32_t vendor_id,
        uint32_t device_features,
        uint32_t queue_num_max)
    {
        constexpr uint32_t MAGIC_VALUE = 0x74726976; // "virt" in little-endian

        VirtIOHeader header{};
        header.magic = MAGIC_VALUE;
        header.version = version;
        header.device_id = device_id;
        header.vendor_id = vendor_id;
        header.device_features = device_features;
        header.queue_num_max = queue_num_max;
        header.status = DeviceStatus::NONE;

        return header;
    }
#endif
};

// 确保内存布局正确
static_assert(offsetof(VirtIOHeader, magic) == 0, "Invalid VirtIOHeader layout");
static_assert(offsetof(VirtIOHeader, version) == 4, "Invalid VirtIOHeader layout");
static_assert(offsetof(VirtIOHeader, device_id) == 8, "Invalid VirtIOHeader layout");


// 网络缓冲区定义
class RxBuffer
{
public:
    RxBuffer(uint16_t idx, size_t len) : idx_(idx), data_(len) {}

    uint16_t idx() const { return idx_; }
    uint8_t *data() { return data_.data(); }
    const uint8_t *data() const { return data_.data(); }
    size_t size() const { return data_.size(); }
    void set_packet_len(size_t len) { packet_len_ = len; }
    size_t packet_len() const { return packet_len_; }

private:
    uint16_t idx_;
    std::vector<uint8_t> data_;
    size_t packet_len_ = 0;
};

class TxBuffer
{
public:
    explicit TxBuffer(size_t len) : data_(len) {}

    uint8_t *data() { return data_.data(); }
    const uint8_t *data() const { return data_.data(); }
    size_t size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
};

// MMIO 传输层
class MmioTransport
{
public:
    explicit MmioTransport(VirtIOHeader *header) : header_(header)
    {
        if (header_->magic != VIRTIO_MAGIC_VALUE)
        {
            panic("Invalid VirtIO magic value");
        }
        if (header_->device_id == 0)
        {
            panic("VirtIO device ID is zero");
        }
    }

    DeviceType device_type() const
    {
        return static_cast<DeviceType>(header_->device_id);
    }

    uint64_t read_device_features()
    {
        header_->device_features_sel = 0;
        uint64_t features = header_->device_features;
        header_->device_features_sel = 1;
        features |= static_cast<uint64_t>(header_->device_features) << 32;
        return features;
    }

    void write_driver_features(uint64_t features)
    {
        header_->driver_features_sel = 0;
        header_->driver_features = static_cast<uint32_t>(features);
        header_->driver_features_sel = 1;
        header_->driver_features = static_cast<uint32_t>(features >> 32);
    }

    uint32_t max_queue_size(uint16_t queue)
    {
        header_->queue_sel = queue;
        return header_->queue_num_max;
    }

    void notify(uint16_t queue)
    {
        header_->queue_notify = queue;
    }

    DeviceStatus get_status() const
    {
        return static_cast<DeviceStatus>(header_->status);
    }

    void set_status(DeviceStatus status)
    {
        header_->status = static_cast<uint8_t>(status);
    }

    void queue_set(uint16_t queue, uint32_t size, uintptr_t descriptors,
                   uintptr_t driver_area, uintptr_t device_area)
    {
        header_->queue_sel = queue;
        header_->queue_num = size;

        // 现代 VirtIO 设置
        header_->queue_desc_low = static_cast<uint32_t>(descriptors);
        header_->queue_desc_high = static_cast<uint32_t>(descriptors >> 32);
        header_->queue_driver_low = static_cast<uint32_t>(driver_area);
        header_->queue_driver_high = static_cast<uint32_t>(driver_area >> 32);
        header_->queue_device_low = static_cast<uint32_t>(device_area);
        header_->queue_device_high = static_cast<uint32_t>(device_area >> 32);
        header_->queue_ready = 1;
    }

    void queue_unset(uint16_t queue)
    {
        header_->queue_sel = queue;
        header_->queue_ready = 0;
        while (header_->queue_ready != 0)
        {
        }

        header_->queue_num = 0;
        header_->queue_desc_low = 0;
        header_->queue_desc_high = 0;
        header_->queue_driver_low = 0;
        header_->queue_driver_high = 0;
        header_->queue_device_low = 0;
        header_->queue_device_high = 0;
    }

    bool ack_interrupt()
    {
        uint32_t interrupt = header_->interrupt_status;
        if (interrupt != 0)
        {
            header_->interrupt_ack = interrupt;
            return true;
        }
        return false;
    }

private:
    VirtIOHeader *header_;
};

// VirtIO 网络设备
template <size_t QUEUE_SIZE>
class VirtIONet
{
public:
    explicit VirtIONet(MmioTransport *transport, size_t buf_len = 2048)
        : transport_(transport), buf_len_(buf_len)
    {
        initialize();
    }

    bool can_send() const
    {
        // 简化实现，实际应根据队列状态判断
        return true;
    }

    bool can_recv() const
    {
        // 简化实现，实际应根据队列状态判断
        return true;
    }

    std::optional<RxBuffer> receive()
    {
        // 简化实现，实际应从队列中接收数据
        RxBuffer buf(0, buf_len_);
        // 这里应该填充实际接收到的数据
        return buf;
    }

    bool send(const uint8_t *data, size_t len)
    {
        if (!data || len == 0)
            return false;

        // 简化实现，实际应通过队列发送数据
        debug("Sending %d bytes", len);
        return true;
    }

    void recycle_rx_buffer(RxBuffer &&buf)
    {
        // 简化实现，实际应回收缓冲区
        rx_buffers_[buf.idx()] = std::move(buf);
    }

private:
    void initialize()
    {
        // 初始化接收队列
        for (size_t i = 0; i < QUEUE_SIZE; ++i)
        {
            rx_buffers_.emplace_back(i, buf_len_);
            // 这里应该将缓冲区添加到接收队列
        }
    }

    MmioTransport *transport_;
    size_t buf_len_;
    std::vector<RxBuffer> rx_buffers_;
};

// 网络设备单例
class NetDeviceSingleton
{
public:
    static NetDeviceSingleton &instance()
    {
        static NetDeviceSingleton inst;
        return inst;
    }

    VirtIONet<32> *device() { return net_device_.get(); }
    void lock() { lock_.acquire(); }
    void unlock() { lock_.release(); }

private:
    NetDeviceSingleton()
    {
        auto header = reinterpret_cast<VirtIOHeader *>(0x10008000);
        auto transport = std::make_unique<MmioTransport>(header);
        net_device_ = std::make_unique<VirtIONet<32>>(transport.get());
    }

    std::unique_ptr<MmioTransport> transport_;
    std::unique_ptr<VirtIONet<32>> net_device_;
    SpinLock lock_;
};

// 辅助宏
#define NET_DEVICE NetDeviceSingleton::instance().device()
#define NET_LOCK NetDeviceSingleton::instance().lock()
#define NET_UNLOCK NetDeviceSingleton::instance().unlock()
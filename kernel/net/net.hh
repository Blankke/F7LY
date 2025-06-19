#pragma once
#include "types.hh"
#include "printer.hh"
#include "spinlock.hh"
#include "virtio_net.hh" // 假设有这个头文件，包含VirtIONet相关定义

enum class NetError
{
    NoData,
    InvalidParam,
    DeviceError
};

class NetDevice
{
public:
    NetDevice() = default;
    explicit NetDevice(uintptr_t ptr)
    {
        // 初始化virtio-net设备
        auto header = reinterpret_cast<VirtIOHeader *>(ptr);
        transport_ = new MmioTransport(header);
        net_ = new VirtIONet<HalImpl>(transport_);
        if (!net_)
        {
            panic("failed to create net driver");
        }
    }

    // Not copyable or movable
    NetDevice(const NetDevice &) = delete;
    NetDevice &operator=(const NetDevice &) = delete;

    size_t recv(uint8_t *buf, size_t len, NetError *err = nullptr)
    {
        if (!buf || len == 0)
        {
            if (err)
                *err = NetError::InvalidParam;
            return 0;
        }

        auto result = net_->recv(buf, len);
        if (result.is_err())
        {
            if (err)
                *err = NetError::NoData;
            return 0;
        }
        return result.unwrap();
    }

    bool send(const uint8_t *buf, size_t len, NetError *err = nullptr)
    {
        if (!buf || len == 0)
        {
            if (err)
                *err = NetError::InvalidParam;
            return false;
        }

        printf("send data %d bytes", len);
        hexdump(buf, len);

        auto result = net_->send(buf, len);
        if (result.is_err())
        {
            if (err)
                *err = NetError::DeviceError;
            return false;
        }
        return true;
    }

    static void hexdump(const uint8_t *data, size_t len)
    {
        constexpr size_t PRELAND_WIDTH = 70;
        printf("{:-^1$}\n", " hexdump ", PRELAND_WIDTH);
        for (size_t offset = 0; offset < len; offset += 16)
        {
            // 打印十六进制
            for (size_t i = 0; i < 16; i++)
            {
                if (offset + i < len)
                    printf("%02x ", data[offset + i]);
                else
                    printf("   ");
            }

            printf("    ");

            // 打印ASCII字符
            for (size_t i = 0; i < 16; i++)
            {
                if (offset + i < len)
                {
                    uint8_t c = data[offset + i];
                    if (c >= 0x20 && c <= 0x7e)
                        printf("%c", c);
                    else
                        printf(".");
                }
            }

            printf("\n");
        }
        printf("{:-^1$}\n", " hexdump end ", PRELAND_WIDTH);
    }

private:
    MmioTransport *transport_;
    VirtIONet<HalImpl> *net_;
};

class NetDeviceSingleton
{
public:
    static NetDeviceSingleton &instance()
    {
        static NetDeviceSingleton inst;
        return inst;
    }

    NetDevice *device() { return &net_device_; }
    void lock() { _lock.acquire(); }
    void unlock() { _lock.release(); }

    NetDeviceSingleton() : net_device_(0x10008000) {}
    ~NetDeviceSingleton() = default;

    NetDevice net_device_;
    SpinLock _lock;
};

// Helper macro for easier access
#define NET_DEVICE NetDeviceSingleton::instance().device()
#define NET_LOCK NetDeviceSingleton::instance().lock()
#define NET_UNLOCK NetDeviceSingleton::instance().unlock()

// 网络栈相关功能可以在这里继续添加
#pragma once

namespace dev
{
enum DeviceType
{
    dev_unknown,
    dev_block,
    dev_char,
    dev_other,
};

// 所有架构共享的最小设备基类。设备类型和就绪/中断语义不依赖 ISA，
// 因此只能在公共 devs 目录维护一份定义。
class VirtualDevice
{
public:
    VirtualDevice() = default;
    virtual ~VirtualDevice() = default;

    virtual DeviceType type() { return DeviceType::dev_unknown; }
    virtual int handle_intr() = 0;
    virtual bool read_ready() = 0;
    virtual bool write_ready() = 0;
};
} // namespace dev

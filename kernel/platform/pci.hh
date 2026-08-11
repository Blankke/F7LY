#pragma once

#include "types.hh"

namespace platform::pci
{
// BDF 只描述设备在 PCI 总线上的位置。ECAM 地址计算、MMIO 窗口和页表
// 映射全部属于所选平台，设备驱动不能保存这些板级细节。
struct FunctionAddress
{
    uint16 bus;
    uint16 device;
    uint16 function;
};

struct MemoryBar
{
    uint64 bus_address;
    uint64 size;
};

// 按 vendor/device ID 查找第 instance 个功能，instance 从 0 开始。
// 枚举遵循 PCIe ECAM 的 4 KiB/function 布局，并识别多功能设备。
bool find_device(uint16 vendor_id, uint16 device_id, uint32 instance,
                 FunctionAddress &result);

// 配置空间访问统一接收 BDF 和 function 内寄存器偏移。平台实现必须检查
// BDF、4 KiB 配置空间边界和自然对齐，禁止驱动拼接裸 ECAM 地址。
uint8 config_read8(FunctionAddress address, uint16 register_offset);
uint16 config_read16(FunctionAddress address, uint16 register_offset);
uint32 config_read32(FunctionAddress address, uint16 register_offset);
void config_read(FunctionAddress address, uint16 register_offset,
                 void *buffer, uint64 length);

void config_write8(FunctionAddress address, uint16 register_offset, uint8 value);
void config_write16(FunctionAddress address, uint16 register_offset, uint16 value);
void config_write32(FunctionAddress address, uint16 register_offset, uint32 value);

// 探测 endpoint 的所有 memory BAR，为 32/64 位 BAR 从平台窗口分配自然
// 对齐的总线地址，并最终只打开 memory decode 与 bus master。此函数应在
// 对应驱动读取 capability 前调用一次；I/O BAR 保持原值且不会被错误启用。
void enable_and_allocate_bars(FunctionAddress address);

// 返回平台为 BAR 编程的设备总线区间。只有已实现的 32/64 位 memory BAR
// 才返回 true；size 是硬件探测出的真实 BAR 大小，不是页表映射粒度。
bool memory_bar(FunctionAddress address, uint8 bar_index, MemoryBar &result);

// 把 capability 中的 BAR 总线区间转换为内核虚拟区间。整个 [address,
// address + size) 都必须属于平台窗口，越界或溢出会直接停止启动。
uint64 mapped_mmio_address(uint64 bus_address, uint64 size);
} // namespace platform::pci

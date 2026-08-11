#pragma once

#include "platform/device_resource.hh"

namespace loongarch::board
{
inline constexpr const char *k_name = "QEMU LoongArch virt";
inline constexpr uint64 k_uart_physical = 0x1fe001e0ULL;
inline constexpr uint32 k_uart_interrupt = 2;
inline constexpr uint32 k_external_cpu_interrupt = 3;
// QEMU virt 的多个 PCI 设备共享同一条 INTx 输入；名字必须描述线路所有权，
// 不能把网卡也使用的 source 错写成“块设备中断”。
inline constexpr uint32 k_pci_intx_interrupt = 32;
inline constexpr platform::MmioRegion k_pch_pic_mmio{
    .physical_base = 0x10000000ULL,
    .size = 0x400ULL,
};
// ExtIOI 是 QEMU LoongArch virt 的外部中断级联资源；2K1000 使用 LIOINTC，
// 因而这些地址不能继续放在架构公共头中。
inline constexpr uint64 k_extioi_enable_physical = 0x1600ULL;
inline constexpr uint64 k_extioi_isr_physical = 0x1800ULL;
inline constexpr uint64 k_extioi_map_physical = 0x14c0ULL;
inline constexpr uint64 k_extioi_route_physical = 0x1c00ULL;
inline constexpr uint64 k_extioi_nodetype_physical = 0x14a0ULL;
// PCI 资源属于 QEMU virt 机器，而不是 LoongArch 架构本身。256 MiB
// generic ECAM 对应 256 个 bus；平台 PCI 层通过未缓存 DMW 访问配置空间，
// 设备驱动只接触 BDF 和 function 内寄存器偏移。
inline constexpr platform::MmioRegion k_pci_ecam{
    .physical_base = 0x20000000ULL,
    .size = 0x10000000ULL,
};

// 现有 VirtIO PCI 驱动从这段窗口顺序分配 BAR。页表只需映射驱动会分配的
// 64 KiB，避免把一个板级地址范围无条件塞进通用 VMM。
inline constexpr platform::MmioRegion k_pci_mmio{
    .physical_base = 0x40000000ULL,
    .size = 16ULL * 4096ULL,
};
inline constexpr uint64 k_pci_mmio_kernel_virtual_base = 0x40000000ULL;
// QEMU virt 沿用当前四级页表可覆盖的 46 位低地址布局。
inline constexpr uint32 k_user_virtual_address_bits = 46;
} // namespace loongarch::board

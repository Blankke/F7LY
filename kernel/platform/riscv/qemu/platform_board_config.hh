#pragma once

#include "platform/device_resource.hh"
#include "types.hh"

namespace riscv::board
{
    // QEMU virt 的固定资源集中在唯一的平台画像。通用驱动只消费这里的
    // typed region/IRQ，不再从 memlayout.hh 读取设备地址。
    inline constexpr platform::MmioRegion k_uart_mmio{
        .physical_base = 0x10000000ULL,
        .size = 0x1000ULL,
    };
    inline constexpr uint32 k_uart_interrupt = 10;

    inline constexpr platform::MmioRegion k_virtio_mmio{
        .physical_base = 0x10001000ULL,
        .size = 8ULL * 0x1000ULL,
    };
    inline constexpr uint64 k_virtio_mmio_stride = 0x1000ULL;
    inline constexpr uint32 k_virtio_mmio_count = 8;
    inline constexpr uint32 k_virtio_interrupt_first = 1;
    inline constexpr uint64 k_primary_block_mmio = 0x10001000ULL;
    inline constexpr uint32 k_primary_block_interrupt = 1;
    inline constexpr uint64 k_secondary_block_mmio = 0x10002000ULL;
    inline constexpr uint32 k_secondary_block_interrupt = 2;

    inline constexpr platform::MmioRegion k_plic_mmio{
        .physical_base = 0x0c000000ULL,
        .size = 0x400000ULL,
    };
    // QEMU virt DTB 的 riscv,ndev=95，硬件 source ID 范围为 1..95。
    inline constexpr uint32 k_plic_source_count = 95;

    // RISC-V 内核页表只映射实际会直接访问的设备。Timer 通过 SBI 编程，
    // 因而无需把 CLINT 暴露给 S-mode 页表。
    inline constexpr platform::MmioRegion k_kernel_mmio_regions[] = {
        k_uart_mmio,
        k_virtio_mmio,
        k_plic_mmio,
    };
} // namespace riscv::board

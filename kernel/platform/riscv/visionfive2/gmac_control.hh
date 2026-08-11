/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "types.hh"

// 驱动只通过这个窄接口接触 JH7110 时钟、reset、MMIO 与非一致性 DMA。
// 这样 DWMAC descriptor/PHY 逻辑不会反向依赖整块 VisionFive 2 画像。
namespace riscv::jh7110::gmac::platform_control
{
bool initialize();
uint64 device_physical_address();
uint32 read(uint32 offset);
void write(uint32 offset, uint32 value);
void io_barrier();
void delay_us(uint64 duration_us);
bool set_speed(uint32 speed_mbps);

// JH7110 GMAC1 的官方 DTS 没有 dma-coherent。下列接口以物理地址执行
// SiFive CCACHE 64-byte flush（writeback + invalidate），普通 fence 不能替代。
bool dma_address(const void *pointer, uint64 size, uint64 &physical_address);
bool dma_sync_for_device(const void *pointer, uint64 size);
bool dma_sync_for_cpu(void *pointer, uint64 size);
} // namespace riscv::jh7110::gmac::platform_control

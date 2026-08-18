#pragma once

#include "platform/device_resource.hh"
#include "types.hh"

namespace riscv::board
{
// JH7110 的 hart0 是 E24 管理核，DTB 中只有 M-mode 外部中断 context；
// F7LY 运行在 U74 的 hart1..4，不能把 hart0 加入 S-mode 调度与 PLIC 拓扑。
constexpr bool is_schedulable_hart(uint64 hart_id)
{
    return hart_id >= 1 && hart_id <= 4;
}

// VisionFive 2/JH7110 的固定 SoC 资源集中在当前画像。DTB 仍是 CPU、RAM、
// PLIC context 与 timebase 的运行时权威来源；这里的地址是板级硬件契约。
inline constexpr platform::MmioRegion k_plic_mmio{
    .physical_base = 0x0c000000ULL,
    .size = 0x04000000ULL,
};
// riscv,ndev=136 表示硬件 source ID 1..136；公共 IRQ registry 仍只管理 <64。
inline constexpr uint32 k_plic_source_count = 136;

inline constexpr platform::MmioRegion k_dw_mmc_mmio{
    .physical_base = 0x16020000ULL,
    .size = 0x00010000ULL,
};
inline constexpr uint64 k_dw_mmc_reference_clock_hz = 50'000'000ULL;

inline constexpr platform::MmioRegion k_gmac1_mmio{
    .physical_base = 0x16040000ULL,
    .size = 0x00010000ULL,
};
inline constexpr platform::MmioRegion k_syscrg_mmio{
    .physical_base = 0x13020000ULL,
    .size = 0x00010000ULL,
};
inline constexpr platform::MmioRegion k_syscon_mmio{
    .physical_base = 0x13030000ULL,
    .size = 0x00001000ULL,
};
inline constexpr platform::MmioRegion k_ccache_mmio{
    .physical_base = 0x02010000ULL,
    .size = 0x00004000ULL,
};

// 本画像只经 SBI 使用 console，因此不映射 UART0。IRQ78 的 GMAC 首版采用
// 轮询，也不扩大公共 64-source 分发表；PLIC 本身仍会清空全部硬件 source。
inline constexpr platform::MmioRegion k_kernel_mmio_regions[] = {
    k_plic_mmio,
    k_dw_mmc_mmio,
    k_gmac1_mmio,
    k_syscrg_mmio,
    k_syscon_mmio,
    k_ccache_mmio,
};

// 在 DWMMC 第一次寄存器访问前准备 JH7110 SDIO1 的 SoC 时钟与复位。
// 通用 DWMMC 驱动只依赖此窄接口，不感知 SYSCRG 的寄存器布局。
bool prepare_dw_mmc_hardware();
} // namespace riscv::board

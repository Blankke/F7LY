/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * JH7110 GMAC1 board control and non-coherent DMA support.
 *
 * Clock/reset layout follows Linux clk-starfive-jh7110-sys/pll and the
 * StarFive DT bindings. Cache maintenance follows Linux sifive_ccache.c:
 * starfive,jh7110-ccache uses a 64-byte FLUSH64 command at offset 0x200.
 */
#include "gmac_control.hh"

#include "mem/page.hh"
#include "platform/memory.hh"
#include "platform_board_config.hh"
#include "printer.hh"
#include "tm/time.hh"
#include "virtual_memory_manager.hh"

namespace riscv::jh7110::gmac::platform_control
{
namespace
{
constexpr uint32 k_syscrg_ahb0 = 0x024;
constexpr uint32 k_syscrg_gmac1_ahb = 0x184;
constexpr uint32 k_syscrg_gmac1_axi = 0x188;
constexpr uint32 k_syscrg_gmac1_gtxclk = 0x190;
constexpr uint32 k_syscrg_gmac1_ptp = 0x198;
constexpr uint32 k_syscrg_gmac1_rx = 0x19c;
constexpr uint32 k_syscrg_gmac1_tx = 0x1a4;
constexpr uint32 k_syscrg_gmac1_gtxc = 0x1ac;
constexpr uint32 k_syscrg_reset_assert2 = 0x300;
constexpr uint32 k_syscrg_reset_status2 = 0x310;

constexpr uint32 k_syscon_gmac1_mode = 0x90;
constexpr uint32 k_syscon_gmac1_mode_mask = 0x7U << 2;
constexpr uint32 k_syscon_gmac1_rgmii = 0x1U << 2;

constexpr uint32 k_clock_divider_mask = 0x00ffffffU;
constexpr uint32 k_clock_mux_mask = 0x0fU << 24;
constexpr uint32 k_clock_enable = 1U << 31;
constexpr uint32 k_gmac_reset_mask = (1U << 2) | (1U << 3);
constexpr uint64 k_reset_timeout_us = 10'000;

constexpr uint32 k_pll0_pd = 0x18;
constexpr uint32 k_pll0_fbdiv = 0x1c;
constexpr uint32 k_pll0_frac = 0x20;
constexpr uint32 k_pll0_prediv = 0x24;
constexpr uint32 k_pll0_dacpd = 1U << 24;
constexpr uint32 k_pll0_dsmpd = 1U << 25;
constexpr uint32 k_pll0_fbdiv_mask = 0x0fffU;
constexpr uint32 k_pll_frac_mask = 0x00ffffffU;
constexpr uint32 k_pll_postdiv_shift = 28;
constexpr uint32 k_pll_postdiv_mask = 0x3U;
constexpr uint32 k_pll_prediv_mask = 0x3fU;
constexpr uint64 k_oscillator_hz = 24'000'000ULL;
constexpr uint64 k_rgmii_1000_tx_clock_hz = 125'000'000ULL;

constexpr uint32 k_ccache_config = 0x000;
constexpr uint32 k_ccache_flush64 = 0x200;
constexpr uint32 k_ccache_block_size_shift = 24;
constexpr uint32 k_ccache_block_size_mask = 0xffU;
constexpr uint64 k_cache_line_size = 64;

bool g_initialized = false;
uint32 g_configured_speed_mbps = 0;

volatile uint32 *register32(const platform::MmioRegion &region, uint32 offset)
{
    return reinterpret_cast<volatile uint32 *>(
        platform::memory::kernel_access_address(region.physical_base) + offset);
}

volatile uint64 *register64(const platform::MmioRegion &region, uint32 offset)
{
    return reinterpret_cast<volatile uint64 *>(
        platform::memory::kernel_access_address(region.physical_base) + offset);
}

uint32 region_read(const platform::MmioRegion &region, uint32 offset)
{
    return *register32(region, offset);
}

void region_write(const platform::MmioRegion &region, uint32 offset, uint32 value)
{
    *register32(region, offset) = value;
}

void full_barrier()
{
    // Linux RISC-V mb() 同样要求 I/O 与内存访问都排在 cache command 两侧。
    asm volatile("fence iorw, iorw" ::: "memory");
}

bool ccache_is_usable()
{
    const uint32 config = region_read(board::k_ccache_mmio, k_ccache_config);
    const uint32 block_log2 =
        (config >> k_ccache_block_size_shift) & k_ccache_block_size_mask;
    if (config == 0 || config == 0xffffffffU || block_log2 >= 63 ||
        (1ULL << block_log2) != k_cache_line_size)
    {
        platformDiagnosticError(
            "[gmac1] unsupported JH7110 CCACHE config=0x%x (need 64-byte lines)\n",
            config);
        return false;
    }
    return true;
}

bool pll0_rate(uint64 &rate_hz)
{
    const uint32 mode = region_read(board::k_syscon_mmio, k_pll0_pd);
    const bool dacpd = (mode & k_pll0_dacpd) != 0;
    const bool dsmpd = (mode & k_pll0_dsmpd) != 0;
    if (dacpd != dsmpd)
    {
        platformDiagnosticError(
            "[gmac1] PLL0 has invalid mixed integer/fraction mode pd=0x%x\n",
            mode);
        return false;
    }

    const uint64 fbdiv =
        region_read(board::k_syscon_mmio, k_pll0_fbdiv) & k_pll0_fbdiv_mask;
    const uint32 frac_reg = region_read(board::k_syscon_mmio, k_pll0_frac);
    const uint64 frac = frac_reg & k_pll_frac_mask;
    const uint32 postdiv =
        (frac_reg >> k_pll_postdiv_shift) & k_pll_postdiv_mask;
    const uint64 prediv =
        region_read(board::k_syscon_mmio, k_pll0_prediv) & k_pll_prediv_mask;
    if (fbdiv == 0 || prediv == 0)
    {
        platformDiagnosticError(
            "[gmac1] invalid PLL0 divisors fbdiv=%lu prediv=%lu\n",
            fbdiv, prediv);
        return false;
    }

    // 与 Linux jh7110_pll_recalc_rate 保持相同的截断顺序。
    uint64 numerator_hz = k_oscillator_hz * fbdiv;
    if (!dacpd)
    {
        numerator_hz += (k_oscillator_hz * frac) / (1ULL << 24);
    }
    rate_hz = numerator_hz / (prediv << postdiv);
    return rate_hz != 0;
}

bool flush_dma_range(const void *pointer, uint64 size)
{
    uint64 physical = 0;
    if (!dma_address(pointer, size, physical))
    {
        platformDiagnosticError(
            "[gmac1] DMA cache sync cannot translate range va=%p size=%lu\n",
            pointer, size);
        return false;
    }
    if (physical > ~0ULL - size)
    {
        return false;
    }

    const uint64 first_line = physical & ~(k_cache_line_size - 1);
    const uint64 end = physical + size;
    volatile uint64 *const flush_command =
        register64(board::k_ccache_mmio, k_ccache_flush64);

    // JH7110 的该命令同时 writeback + invalidate；Linux 将它用于
    // wback、invalidate 和 wback_invalidate 三种 nonstandard cache op。
    full_barrier();
    for (uint64 line = first_line; line < end; line += k_cache_line_size)
    {
        *flush_command = line;
    }
    full_barrier();
    return true;
}
} // namespace

uint32 read(uint32 offset)
{
    return region_read(board::k_gmac1_mmio, offset);
}

uint64 device_physical_address()
{
    return board::k_gmac1_mmio.physical_base;
}

void write(uint32 offset, uint32 value)
{
    region_write(board::k_gmac1_mmio, offset, value);
}

void io_barrier()
{
    full_barrier();
}

void delay_us(uint64 duration_us)
{
    if (duration_us == 0)
    {
        return;
    }
    uint64 duration_ticks = tmm::microseconds_to_cycles(duration_us);
    if (duration_ticks == 0)
    {
        duration_ticks = 1;
    }
    const uint64 start = tmm::get_hw_time_stamp();
    while (tmm::get_hw_time_stamp() - start < duration_ticks)
    {
        asm volatile("nop");
    }
}

bool set_speed(uint32 speed_mbps)
{
    if (speed_mbps != 1000)
    {
        // GTXCLK 的硬件 divider 只有 1..15。当前没有可靠资料证明所有
        // PLL0 配置都能无误生成 25/2.5 MHz，故不能假装支持 100/10 Mbit。
        platformDiagnosticError(
            "[gmac1] unsupported RGMII link speed=%u Mbps; only 1000 Mbps clocking is verified\n",
            speed_mbps);
        return false;
    }
    if (g_configured_speed_mbps == speed_mbps)
    {
        return true;
    }

    uint64 pll_rate_hz = 0;
    if (!pll0_rate(pll_rate_hz) ||
        pll_rate_hz % k_rgmii_1000_tx_clock_hz != 0)
    {
        platformDiagnosticError(
            "[gmac1] PLL0=%lu Hz cannot exactly produce 125 MHz RGMII clock\n",
            pll_rate_hz);
        return false;
    }
    const uint64 divider = pll_rate_hz / k_rgmii_1000_tx_clock_hz;
    if (divider == 0 || divider > 15)
    {
        platformDiagnosticError(
            "[gmac1] PLL0=%lu Hz requires invalid GTXCLK divider=%lu\n",
            pll_rate_hz, divider);
        return false;
    }

    const uint32 old_clock =
        region_read(board::k_syscrg_mmio, k_syscrg_gmac1_gtxclk);
    const uint32 new_clock =
        (old_clock & ~k_clock_divider_mask) | static_cast<uint32>(divider);
    region_write(board::k_syscrg_mmio, k_syscrg_gmac1_gtxclk, new_clock);
    full_barrier();
    const uint32 readback =
        region_read(board::k_syscrg_mmio, k_syscrg_gmac1_gtxclk);
    if ((readback & k_clock_divider_mask) != divider)
    {
        platformDiagnosticError(
            "[gmac1] GTXCLK divider write failed wanted=%lu read=0x%x\n",
            divider, readback);
        return false;
    }

    g_configured_speed_mbps = speed_mbps;
    platformDiagnosticInfo(
        "[gmac1] PLL0=%lu Hz GTXCLK divider=%lu rate=125000000 Hz\n",
        pll_rate_hz, divider);
    return true;
}

bool initialize()
{
    if (g_initialized)
    {
        return true;
    }
    if (!ccache_is_usable())
    {
        // 没有可验证 cache op 时直接拒绝启动；fence 只排序，不能修复
        // cached BSS 与非一致性 GMAC DMA 之间的数据可见性。
        return false;
    }

    uint32 mode = region_read(board::k_syscon_mmio, k_syscon_gmac1_mode);
    mode = (mode & ~k_syscon_gmac1_mode_mask) | k_syscon_gmac1_rgmii;
    region_write(board::k_syscon_mmio, k_syscon_gmac1_mode, mode);

    // GMAC1_TX 选择 parent0(GTXCLK)，RX 选择 parent0(RGMII_RXIN)。
    uint32 rx_clock = region_read(board::k_syscrg_mmio, k_syscrg_gmac1_rx);
    region_write(board::k_syscrg_mmio, k_syscrg_gmac1_rx,
                 rx_clock & ~k_clock_mux_mask);
    uint32 tx_clock = region_read(board::k_syscrg_mmio, k_syscrg_gmac1_tx);
    region_write(board::k_syscrg_mmio, k_syscrg_gmac1_tx,
                 (tx_clock & ~k_clock_mux_mask) | k_clock_enable);

    const uint32 gate_offsets[] = {
        k_syscrg_ahb0,
        k_syscrg_gmac1_ahb,
        k_syscrg_gmac1_axi,
        k_syscrg_gmac1_ptp,
        k_syscrg_gmac1_gtxc,
    };
    for (const uint32 offset : gate_offsets)
    {
        region_write(board::k_syscrg_mmio, offset,
                     region_read(board::k_syscrg_mmio, offset) |
                         k_clock_enable);
    }
    if (!set_speed(1000))
    {
        return false;
    }
    full_barrier();

    const uint32 reset =
        region_read(board::k_syscrg_mmio, k_syscrg_reset_assert2);
    region_write(board::k_syscrg_mmio, k_syscrg_reset_assert2,
                 reset & ~k_gmac_reset_mask);
    full_barrier();

    const uint64 start = tmm::get_hw_time_stamp();
    const uint64 timeout = tmm::microseconds_to_cycles(k_reset_timeout_us);
    while ((region_read(board::k_syscrg_mmio, k_syscrg_reset_status2) &
            k_gmac_reset_mask) != k_gmac_reset_mask)
    {
        if (tmm::get_hw_time_stamp() - start >= timeout)
        {
            platformDiagnosticError(
                "[gmac1] reset deassert timeout assert=0x%x status=0x%x\n",
                region_read(board::k_syscrg_mmio, k_syscrg_reset_assert2),
                region_read(board::k_syscrg_mmio, k_syscrg_reset_status2));
            return false;
        }
    }

    g_initialized = true;
    platformDiagnosticInfo(
        "[gmac1] platform ready mode=0x%x reset-status=0x%x ccache=0x%x\n",
        region_read(board::k_syscon_mmio, k_syscon_gmac1_mode),
        region_read(board::k_syscrg_mmio, k_syscrg_reset_status2),
        region_read(board::k_ccache_mmio, k_ccache_config));
    return true;
}

bool dma_address(const void *pointer, uint64 size, uint64 &physical_address)
{
    if (pointer == nullptr || size == 0)
    {
        return false;
    }
    const uint64 virtual_start = reinterpret_cast<uint64>(pointer);
    if (virtual_start > ~0ULL - (size - 1))
    {
        return false;
    }
    const uint64 virtual_end = virtual_start + size;
    const uint64 first_physical = mem::k_pagetable.kwalk_addr(virtual_start);
    if (first_physical == 0 || first_physical > ~0ULL - (size - 1))
    {
        return false;
    }

    // descriptor/buffer 必须在物理上连续；不能只翻译第一页后假设后续页相邻。
    uint64 page = (virtual_start & ~(static_cast<uint64>(PGSIZE) - 1)) + PGSIZE;
    while (page < virtual_end)
    {
        const uint64 page_physical = mem::k_pagetable.kwalk_addr(page);
        if (page_physical == 0 ||
            page_physical != first_physical + (page - virtual_start))
        {
            return false;
        }
        page += PGSIZE;
    }
    physical_address = first_physical;
    return true;
}

bool dma_sync_for_device(const void *pointer, uint64 size)
{
    return flush_dma_range(pointer, size);
}

bool dma_sync_for_cpu(void *pointer, uint64 size)
{
    return flush_dma_range(pointer, size);
}
} // namespace riscv::jh7110::gmac::platform_control

/* SPDX-License-Identifier: GPL-3.0-only */
#include "vf2_gmac_platform.hh"

#include "libs/printer.hh"
#ifdef VISIONFIVE2
#include "hal/riscv/rv_csr.hh"
#endif
#include "mem/memlayout.hh"
#include "virtual_memory_manager.hh"

namespace net::vf2
{
#ifdef VISIONFIVE2
    namespace
    {
        constexpr uint32 k_syscrg_gmac1_ahb = 0x184;
        constexpr uint32 k_syscrg_gmac1_axi = 0x188;
        constexpr uint32 k_syscrg_ahb0 = 0x024;
        constexpr uint32 k_syscrg_gmac_src = 0x18c;
        constexpr uint32 k_syscrg_gmac1_gtxclk = 0x190;
        constexpr uint32 k_syscrg_gmac1_rmii_rtx = 0x194;
        constexpr uint32 k_syscrg_gmac1_ptp = 0x198;
        constexpr uint32 k_syscrg_gmac1_rx = 0x19c;
        constexpr uint32 k_syscrg_gmac1_rx_inv = 0x1a0;
        constexpr uint32 k_syscrg_gmac1_tx = 0x1a4;
        constexpr uint32 k_syscrg_gmac1_tx_inv = 0x1a8;
        constexpr uint32 k_syscrg_gmac1_gtxc = 0x1ac;
        constexpr uint32 k_syscrg_reset2 = 0x300;
        constexpr uint32 k_syscrg_reset_status2 = 0x310;
        constexpr uint32 k_syscon_gmac1_mode = 0x90;
        constexpr uint32 k_rgmii_mode_mask = 0x7u << 2;
        constexpr uint32 k_rgmii_mode = 0x1u << 2;
        constexpr uint32 k_clk_enable = 1u << 31;
        constexpr uint32 k_clk_mux_mask = 0x0fu << 24;
        constexpr uint32 k_ccache_config = 0x000;
        constexpr uint32 k_ccache_flush64 = 0x200;
        constexpr uint32 k_ccache_block_size_shift = 24;
        constexpr uint32 k_ccache_block_size_mask = 0xffu;
        constexpr uint64 k_cache_line_size = 64;

        inline volatile uint32 *reg_ptr(uint64 base, uint32 offset)
        {
            return reinterpret_cast<volatile uint32 *>(base + offset);
        }

        bool ccache_is_usable()
        {
            uint32 config = read_reg(VF2_CCACHE_BASE_V, k_ccache_config);
            uint32 block_log2 =
                (config >> k_ccache_block_size_shift) & k_ccache_block_size_mask;
            if (config == 0 || config == 0xffffffffu || block_log2 >= 63 ||
                (1ull << block_log2) != k_cache_line_size)
            {
                printf("[vf2_gmac] unsupported CCACHE config=0x%x (need 64-byte lines)\n",
                       config);
                return false;
            }
            printf("[vf2_gmac] CCACHE config=0x%x line_size=%u\n", config,
                   static_cast<uint32>(k_cache_line_size));
            return true;
        }

        bool flush_dma_range(const void *ptr, uint64 size)
        {
            if (ptr == nullptr || size == 0)
                return false;
            uint64 start = reinterpret_cast<uint64>(ptr);
            if (start > ~0ull - size)
                return false;
            uint64 end = start + size;
            uint64 line = start & ~(k_cache_line_size - 1);
            volatile uint64 *flush = reinterpret_cast<volatile uint64 *>(
                VF2_CCACHE_BASE_V + k_ccache_flush64);

            io_fence();
            for (; line < end; line += k_cache_line_size)
            {
                uint64 physical = dma_address(reinterpret_cast<const void *>(line));
                if (physical == 0)
                {
                    io_fence();
                    return false;
                }
                *flush = physical & ~(k_cache_line_size - 1);
            }
            io_fence();
            return true;
        }

    }

    uint32 read_reg(uint64 base, uint32 offset)
    {
        return *reg_ptr(base, offset);
    }

    void write_reg(uint64 base, uint32 offset, uint32 value)
    {
        *reg_ptr(base, offset) = value;
    }

    void delay_us(uint32 usec)
    {
        for (volatile uint32 i = 0; i < usec * 32; ++i)
        {
            asm volatile("nop");
        }
    }

    void io_fence()
    {
        asm volatile("fence iorw, iorw" ::: "memory");
    }

    uint64 dma_address(const void *ptr)
    {
        uint64 va = reinterpret_cast<uint64>(ptr);
        uint64 pa = mem::k_pagetable.kwalk_addr(va);
        return pa != 0 ? pa : riscv::virt_to_phy_address(va);
    }

    bool dma_sync_for_device(const void *ptr, uint64 size)
    {
        return flush_dma_range(ptr, size);
    }

    bool dma_sync_for_cpu(void *ptr, uint64 size)
    {
        return flush_dma_range(ptr, size);
    }

    bool platform_init()
    {
        if (!ccache_is_usable())
            return false;

        const uint32 ahb0_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_ahb0);
        const uint32 ahb_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_ahb);
        const uint32 axi_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_axi);
        const uint32 src_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac_src);
        const uint32 gtxclk_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_gtxclk);
        const uint32 rmii_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rmii_rtx);
        const uint32 ptp_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_ptp);
        const uint32 rx_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx);
        const uint32 rxinv_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx_inv);
        const uint32 tx_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx);
        const uint32 txinv_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx_inv);
        const uint32 gtxc_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_gtxc);
        const uint32 reset_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset2);
        const uint32 reset_status_before =
            read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset_status2);
        printf("[vf2_gmac] platform clocks before ahb0=0x%x ahb=0x%x axi=0x%x src=0x%x gtxclk=0x%x rmii=0x%x ptp=0x%x rx=0x%x rxinv=0x%x tx=0x%x txinv=0x%x gtxc=0x%x reset2=0x%x status2=0x%x\n",
               ahb0_before, ahb_before, axi_before, src_before, gtxclk_before,
               rmii_before, ptp_before, rx_before, rxinv_before, tx_before,
               txinv_before, gtxc_before, reset_before, reset_status_before);

        uint32 mode = read_reg(VF2_SYS_SYSCON_BASE_V, k_syscon_gmac1_mode);
        mode = (mode & ~k_rgmii_mode_mask) | k_rgmii_mode;
        write_reg(VF2_SYS_SYSCON_BASE_V, k_syscon_gmac1_mode, mode);
        io_fence();

        // Linux maps each SYSCRG clock ID to base + 4 * ID. Only GATE/GDIV
        // clocks have the bit31 enable field.
        const uint32 clock_offsets[] = {k_syscrg_ahb0, k_syscrg_gmac1_ahb,
                                        k_syscrg_gmac1_axi,
                                        k_syscrg_gmac1_ptp,
                                        k_syscrg_gmac1_gtxc};
        for (uint32 offset : clock_offsets)
        {
            uint32 clock = read_reg(VF2_SYSCRG_BASE_V, offset);
            write_reg(VF2_SYSCRG_BASE_V, offset, clock | k_clk_enable);
        }
        // Preserve the TX clock source selected by U-Boot. Its value is
        // link-speed dependent; forcing a mux parent here can disconnect the
        // MAC from the PHY even though DMA keeps completing descriptors.
        uint32 rx_clock = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx);
        write_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx,
                  rx_clock & ~k_clk_mux_mask);
        io_fence();
        write_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset2,
                  reset_before & ~((1u << 2) | (1u << 3)));
        io_fence();
        // Allow reset deassertion to propagate through the GMAC clock domain.
        delay_us(10);
        uint32 reset_status = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset_status2);
        printf("[vf2_gmac] platform clocks after ahb0=0x%x ahb=0x%x axi=0x%x src=0x%x gtxclk=0x%x rmii=0x%x ptp=0x%x rx=0x%x rxinv=0x%x tx=0x%x txinv=0x%x gtxc=0x%x reset2=0x%x->0x%x status2=0x%x\n",
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_ahb0),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_ahb),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_axi),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac_src),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_gtxclk),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rmii_rtx),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_ptp),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx_inv),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx_inv),
               read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_gtxc),
               reset_before, read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset2), reset_status);
        return true;
    }

    void platform_set_speed(uint32 speed_mbps)
    {
        // The clock rate is intentionally left unchanged while the 100 Mbps
        // JH7110 RGMII clock configuration is being measured against U-Boot.
        (void)speed_mbps;
    }
#else
    uint32 read_reg(uint64, uint32) { return 0; }
    void write_reg(uint64, uint32, uint32) {}
    void delay_us(uint32) {}
    void io_fence() {}
    uint64 dma_address(const void *) { return 0; }
    bool platform_init() { return false; }
    void platform_set_speed(uint32) {}
#endif
}

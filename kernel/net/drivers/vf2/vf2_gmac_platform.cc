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

        inline volatile uint32 *reg_ptr(uint64 base, uint32 offset)
        {
            return reinterpret_cast<volatile uint32 *>(base + offset);
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

    bool platform_init()
    {
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
        // GMAC1_RX parent 0 is RGMII_RXIN; GMAC1_TX parent 0 is GTXCLK.
        uint32 rx_clock = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx);
        write_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_rx,
                  rx_clock & ~k_clk_mux_mask);
        uint32 tx_clock = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx);
        write_reg(VF2_SYSCRG_BASE_V, k_syscrg_gmac1_tx,
                  (tx_clock & ~k_clk_mux_mask) | k_clk_enable);
        io_fence();
        uint32 reset_before = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset2);
        write_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset2,
                  reset_before & ~((1u << 2) | (1u << 3)));
        io_fence();
        // Allow reset deassertion to propagate through the GMAC clock domain.
        delay_us(10);
        uint32 reset_status = read_reg(VF2_SYSCRG_BASE_V, k_syscrg_reset_status2);
        printf("[vf2_gmac] platform clocks ahb0=0x%x ahb=0x%x axi=0x%x src=0x%x gtxclk=0x%x rmii=0x%x ptp=0x%x rx=0x%x rxinv=0x%x tx=0x%x txinv=0x%x gtxc=0x%x reset2=0x%x->0x%x status2=0x%x\n",
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
        // Clock parent switching is intentionally deferred until board timing is verified.
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

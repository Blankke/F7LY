#include "ls2k1000_liointc.hh"

#include "hal/loongarch/platform_board.hh"
#include "printer.hh"

#ifdef BOARD_LS2K1000
namespace loongarch::liointc
{
namespace
{
    constexpr uint64 k_route_offset = 0x00;
    constexpr uint64 k_enable_offset = 0x28;
    constexpr uint64 k_disable_offset = 0x2c;
    constexpr uint64 k_polarity_offset = 0x30;
    constexpr uint64 k_edge_offset = 0x34;
    constexpr uint32 k_cpu_hwi_base = 2;

    uint32 g_enabled_inputs = 0;

    volatile uint8 *registers()
    {
        return reinterpret_cast<volatile uint8 *>(
            board::mmio_address(board::k_liointc_registers_physical));
    }

    volatile uint8 *isr()
    {
        return reinterpret_cast<volatile uint8 *>(
            board::mmio_address(board::k_liointc_isr_physical));
    }

    void write32(uint64 offset, uint32 value)
    {
        *reinterpret_cast<volatile uint32 *>(registers() + offset) = value;
    }
}

void init(uint32 enabled_inputs)
{
    const uint32 parent_index = board::k_external_cpu_interrupt - k_cpu_hwi_base;
    if (parent_index >= 4)
    {
        panic("LS2K1000 LIOINTC cascade IRQ 配置越界: %u", board::k_external_cpu_interrupt);
    }

    const uint8 route = static_cast<uint8>((1U << 0) | (1U << (4 + parent_index)));
    for (uint32 input = 0; input < board::k_liointc_input_count; ++input)
    {
        registers()[k_route_offset + input] = route;
    }

    // LIOINTC 的 enable/disable 是 W1S 寄存器；设备均按 DTB 使用高电平电平触发。
    write32(k_disable_offset, 0xffffffffU);
    write32(k_edge_offset, 0);
    write32(k_polarity_offset, 0);
    asm volatile("dbar 0" ::: "memory");

    g_enabled_inputs = enabled_inputs;
    write32(k_enable_offset, enabled_inputs);
    asm volatile("dbar 0" ::: "memory");
    printfGreen("[liointc] enabled=0x%x cascade=%u route=0x%x\n",
                enabled_inputs, board::k_external_cpu_interrupt, route);
}

uint32 claim()
{
    const uint32 pending = *reinterpret_cast<volatile uint32 *>(isr());
    return pending & g_enabled_inputs;
}

void complete(uint32 inputs)
{
    (void)inputs;
    // 所有已启用输入均为电平触发，由设备处理函数清除源端条件后自然撤销。
}
} // namespace loongarch::liointc
#endif

#pragma once

#include "platform_board_constants.h"

namespace loongarch::board
{
inline constexpr const char *k_name = "Loongson 2K1000";
inline constexpr uint64 k_uart_physical = LS2K1000_UART_PHYSICAL;
inline constexpr uint32 k_uart_interrupt = 0;

inline constexpr uint64 k_liointc_registers_physical = 0x1fe01400ULL;
inline constexpr uint64 k_liointc_isr_physical = 0x1fe01040ULL;
inline constexpr uint32 k_liointc_input_count = 32;
// JL-LSGD2K10 DTB 把 LIOINTC 级联到 CPU HWI1，即架构中断号 3。
inline constexpr uint32 k_external_cpu_interrupt = 3;

inline constexpr uint64 k_ahci_physical = 0x400e0000ULL;
inline constexpr uint32 k_ahci_interrupt = 19;
inline constexpr uint32 k_ahci_port_fallback = 1;
inline constexpr uint64 k_gmac0_physical = 0x40040000ULL;
inline constexpr uint32 k_gmac0_interrupt = 12;
inline constexpr uint64 k_rtc_physical = 0x1fe27800ULL;
// 2K1000 只实现 40 位虚拟地址；用户特殊页必须留在硬件可接受的低地址区。
inline constexpr uint32 k_user_virtual_address_bits = 39;
} // namespace loongarch::board

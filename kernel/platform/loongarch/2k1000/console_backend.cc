#include "platform/console_backend.hh"

#include "devs/ns16550.hh"
#include "hal/loongarch/platform_board.hh"

namespace platform::console_backend
{
namespace
{
    dev::serial::Ns16550 g_uart;
}

bool initialize()
{
    // 2K1000 串口输入时钟来自板级时钟树。当前内核尚未从 DTB 解析该时钟，
    // 所以只设置 8N1/FIFO/中断，并保留 U-Boot 已建立的 divisor。
    return g_uart.initialize(loongarch::board::k_uart_mmio, true);
}

bool try_getc(uint8 &character)
{
    return g_uart.try_read(character);
}

bool try_putc(uint8 character)
{
    return g_uart.try_write(character);
}

void set_transmit_interrupt_enabled(bool enabled)
{
    g_uart.set_transmit_interrupt_enabled(enabled);
}

void flush_input()
{
    g_uart.flush_input();
}

void flush_output()
{
    g_uart.flush_output();
}

LineStatus line_status()
{
    const dev::serial::Ns16550::LineStatus status = g_uart.line_status();
    return {
        .transmitter_empty = status.transmitter_empty,
        .raw = status.raw,
        .new_errors = status.new_errors,
    };
}

InterruptSource interrupt_source()
{
    return {
        .present = true,
        .source = loongarch::board::k_uart_interrupt,
    };
}

const char *name()
{
    return "ls2k1000-ns16550";
}
} // namespace platform::console_backend

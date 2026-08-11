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
    return g_uart.initialize(loongarch::board::k_uart_mmio, false);
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
    return "qemu-ns16550";
}
} // namespace platform::console_backend

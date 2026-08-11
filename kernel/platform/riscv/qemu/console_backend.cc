#include "platform/console_backend.hh"

#include "devs/ns16550.hh"
#include "hal/riscv/platform_board.hh"
#include "hal/riscv/sbi.hh"

namespace platform::console_backend
{
namespace
{
    dev::serial::Ns16550 g_uart;
}

void early_write(const char *message)
{
    // start.cc 可能在全局构造前调用；这里只能直接使用无状态 SBI ABI。
    while (message != nullptr && *message != '\0')
    {
        sbi_console_putchar(*message++);
    }
}

bool initialize()
{
    // QEMU virt 保留项目已经验证过的混合契约：MMIO 负责发送与 IRQ，
    // OpenSBI legacy console 负责接收。该特殊性只能存在于本平台后端。
    return g_uart.initialize(riscv::board::k_uart_mmio.physical_base, false);
}

bool try_getc(uint8 &character)
{
    const int value = sbi_console_getchar();
    if (value < 0)
    {
        return false;
    }
    character = static_cast<uint8>(value);
    return true;
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
    // legacy SBI 没有 flush 操作，只能取走当前固件队列中的全部字节。
    while (sbi_console_getchar() >= 0)
    {
    }
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
        .source = riscv::board::k_uart_interrupt,
    };
}

const char *name()
{
    return "qemu-ns16550-tx+sbi-rx";
}
} // namespace platform::console_backend

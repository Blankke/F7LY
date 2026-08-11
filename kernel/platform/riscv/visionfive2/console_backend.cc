#include "platform/console_backend.hh"

#include "hal/riscv/sbi.hh"

namespace platform::console_backend
{
void early_write(const char *message)
{
    // 该入口可能发生在 BSS 清零和全局构造之前，禁止访问锁、队列或静态状态。
    while (message != nullptr && *message != '\0')
    {
        sbi_console_putchar(*message++);
    }
}

bool initialize()
{
    return true;
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
    sbi_console_putchar(character);
    return true;
}

void set_transmit_interrupt_enabled(bool enabled)
{
    (void)enabled;
}

void flush_input()
{
    // legacy SBI 没有 flush；持续读取直到固件输入队列为空。
    while (sbi_console_getchar() >= 0)
    {
    }
}

void flush_output()
{
    // legacy SBI putchar 是同步调用，没有可由 S-mode 丢弃的硬件队列。
}

LineStatus line_status()
{
    return {
        .transmitter_empty = true,
        .raw = 0,
        .new_errors = 0,
    };
}

InterruptSource interrupt_source()
{
    return {
        .present = false,
        .source = 0,
    };
}

const char *name()
{
    return "opensbi-legacy-console";
}
} // namespace platform::console_backend

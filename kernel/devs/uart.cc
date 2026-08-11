#include "uart.hh"

#include "console.hh"
#include "device_manager.hh"
#include "platform/console_backend.hh"
#include "printer.hh"

namespace dev
{
namespace
{
    void report_line_errors(const platform::console_backend::LineStatus &status)
    {
        if (status.new_errors == 0)
        {
            return;
        }

        // 后端只会把每一种新出现的错误位上报一次，既保留实机排障线索，
        // 又不会让损坏的串口线路持续刷屏并掩盖后续启动阶段。
        boardPrintfError(
            "[console] backend=%s line-error raw=0x%x new=0x%x\n",
            platform::console_backend::name(), status.raw, status.new_errors);
    }
} // namespace

UartManager k_uart;

void register_debug_uart(CharDevice *uart_port)
{
    k_devm.register_char_device(uart_port, DEFAULT_DEBUG_CONSOLE_NAME);
    k_stdin.redirect_stream(uart_port);
    k_stdout.redirect_stream(uart_port);
    k_stderr.redirect_stream(uart_port);
}

bool UartManager::initialize()
{
    _lock.init("uart");
    _write_index = 0;
    _read_index = 0;
    _pending_input_valid = false;
    _pending_input = 0;
    return platform::console_backend::initialize();
}

bool UartManager::read_ready()
{
    _lock.acquire();
    if (!_pending_input_valid)
    {
        _pending_input_valid =
            platform::console_backend::try_getc(_pending_input);
    }
    const bool ready = _pending_input_valid;
    _lock.release();
    return ready;
}

bool UartManager::write_ready()
{
    _lock.acquire();
    const bool ready = !output_buffer_full();
    _lock.release();
    return ready;
}

int UartManager::put_char_sync(u8 character)
{
    // 同步输出也进入唯一发送队列，不能越过之前已经排队的字符。
    if (put_char(character) < 0)
    {
        return -1;
    }

    for (;;)
    {
        _lock.acquire();
        start_transmit_locked();
        const bool submitted = _write_index == _read_index;
        _lock.release();
        if (submitted)
        {
            return 0;
        }
        asm volatile("nop");
    }
}

int UartManager::put_char(u8 character)
{
    if (k_printer.is_panic())
    {
        for (;;)
        {
            asm volatile("nop");
        }
    }

    for (;;)
    {
        _lock.acquire();
        if (output_buffer_full())
        {
            // 主动推进一次后仍满，就释放锁等待 TX-ready。拿锁自旋会让
            // 中断处理函数永远无法排空同一队列。
            start_transmit_locked();
            if (output_buffer_full())
            {
                _lock.release();
                asm volatile("nop");
                continue;
            }
        }

        _output_buffer[_write_index % k_output_buffer_size] =
            static_cast<char>(character);
        ++_write_index;
        start_transmit_locked();
        _lock.release();
        return 0;
    }
}

int UartManager::get_char_sync(u8 *character)
{
    if (character == nullptr)
    {
        return -1;
    }

    // 每次失败都已释放锁，RX 中断可以并发把字节送入 Console 行规程。
    while (get_char(character) < 0)
    {
        asm volatile("nop");
    }
    return 0;
}

int UartManager::get_char(u8 *character)
{
    if (character == nullptr)
    {
        return -1;
    }

    _lock.acquire();
    if (_pending_input_valid)
    {
        *character = _pending_input;
        _pending_input_valid = false;
        _lock.release();
        return 0;
    }

    const bool ready = platform::console_backend::try_getc(*character);
    _lock.release();
    return ready ? 0 : -1;
}

void UartManager::start_transmit_locked()
{
    while (_read_index != _write_index)
    {
        const u8 character = static_cast<u8>(
            _output_buffer[_read_index % k_output_buffer_size]);
        if (!platform::console_backend::try_putc(character))
        {
            // 后端仍忙时只打开通知；软件队列及索引始终归本类所有。
            platform::console_backend::set_transmit_interrupt_enabled(true);
            return;
        }
        ++_read_index;
    }

    // 队列已经全部交给硬件，关闭 TX 空中断以避免无意义的中断风暴。
    platform::console_backend::set_transmit_interrupt_enabled(false);
}

int UartManager::handle_intr()
{
    for (;;)
    {
        u8 character = 0;
        _lock.acquire();
        bool received = false;
        if (_pending_input_valid)
        {
            // read_ready() 为 SBI 等破坏性探测保存的字节仍属于输入流头部。
            // IRQ 必须先提交它，不能越过它去读取后续硬件字节而打乱顺序。
            character = _pending_input;
            _pending_input_valid = false;
            received = true;
        }
        else
        {
            received = platform::console_backend::try_getc(character);
        }
        start_transmit_locked();
        const platform::console_backend::LineStatus status =
            platform::console_backend::line_status();
        _lock.release();

        report_line_errors(status);
        if (!received)
        {
            break;
        }

        // 硬件 FIFO 只在 UART 锁内消费；Console 回显可能重新进入发送路径，
        // 因而必须释放 UART 锁后再进入行规程，避免 UART -> Console -> UART。
        kConsole.console_intr(character);
    }
    return 0;
}

int UartManager::get_input_buffer_size()
{
    // try_getc 对 SBI 等传输是破坏性读取，read_ready 会把结果保存到预读槽。
    return read_ready() ? 1 : 0;
}

int UartManager::get_output_buffer_size()
{
    _lock.acquire();
    const int buffered = static_cast<int>(_write_index - _read_index);
    _lock.release();
    return buffered;
}

int UartManager::flush_buffer(int queue)
{
    _lock.acquire();
    switch (queue)
    {
    case 0: // TCIFLUSH
        _pending_input_valid = false;
        platform::console_backend::flush_input();
        break;
    case 1: // TCOFLUSH
        _write_index = _read_index = 0;
        platform::console_backend::set_transmit_interrupt_enabled(false);
        platform::console_backend::flush_output();
        break;
    case 2: // TCIOFLUSH
        _pending_input_valid = false;
        _write_index = _read_index = 0;
        platform::console_backend::set_transmit_interrupt_enabled(false);
        platform::console_backend::flush_input();
        platform::console_backend::flush_output();
        break;
    default:
        _lock.release();
        return -1;
    }
    _lock.release();
    return 0;
}

int UartManager::get_line_status()
{
    _lock.acquire();
    const platform::console_backend::LineStatus status =
        platform::console_backend::line_status();
    const bool software_queue_empty = _write_index == _read_index;
    _lock.release();

    report_line_errors(status);
    // TIOCSER_TEMT 只有在软件队列和物理移位寄存器都为空时才成立。
    return software_queue_empty && status.transmitter_empty ? 0x01 : 0;
}
} // namespace dev

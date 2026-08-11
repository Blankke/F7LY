#pragma once

#include "types.hh"

namespace dev::serial
{
    /**
     * NS16550 的可复用寄存器核心。
     *
     * 本类只拥有 MMIO 寄存器语义，不拥有锁、软件队列、中断注册或 Console
     * 行规程。调用方必须保证 base 是已经可访问的、字节宽寄存器窗口，并串行化
     * 所有成员调用。平台后端负责提供该能力边界。
     */
    class Ns16550
    {
    public:
        struct LineStatus
        {
            bool transmitter_empty;
            uint8 raw;
            uint8 new_errors;
        };

        bool initialize(uint64 base, bool preserve_firmware_divisor)
        {
            if (base == 0)
            {
                return false;
            }

            _base = base;
            _observed_line_errors = 0;
            _reported_line_errors = 0;

            // 先明确关闭 DLAB，再访问 offset 1。固件通常已这样收尾，但若它
            // 意外留下 DLAB=1，直接写 IER 会破坏 divisor high，违背实板的
            // “保留固件波特率”契约。
            write(Register::LineControl, k_word_length_8);
            write(Register::InterruptEnable, 0);
            if (!preserve_firmware_divisor)
            {
                // QEMU 的 1.8432 MHz 输入时钟使用 divisor=3 得到 38400 baud。
                // 实板若由固件建立线路时钟，平台后端必须选择 preserve=true。
                write(Register::LineControl, k_divisor_latch_access);
                write(Register::ReceiverBuffer, 0x03); // divisor low
                write(Register::InterruptEnable, 0x00); // divisor high
            }

            write(Register::LineControl, k_word_length_8);
            write(Register::FifoControl,
                  k_fifo_enable | k_fifo_clear_rx | k_fifo_clear_tx);
            write(Register::InterruptEnable, k_interrupt_rx_ready);
            observe_line_errors(read(Register::LineStatus));
            return true;
        }

        bool try_read(uint8 &character)
        {
            const uint8 status = read(Register::LineStatus);
            observe_line_errors(status);
            if ((status & k_line_data_ready) == 0)
            {
                return false;
            }
            character = read(Register::ReceiverBuffer);
            return true;
        }

        bool try_write(uint8 character)
        {
            const uint8 status = read(Register::LineStatus);
            observe_line_errors(status);
            if ((status & k_line_transmit_holding_empty) == 0)
            {
                return false;
            }
            write(Register::TransmitterHolding, character);
            return true;
        }

        void set_transmit_interrupt_enabled(bool enabled)
        {
            write(Register::InterruptEnable,
                  k_interrupt_rx_ready |
                      (enabled ? k_interrupt_transmit_empty : 0));
        }

        void flush_input()
        {
            // FIFO clear 是一次性命令；保持 FIFO 使能，且不误清发送 FIFO。
            write(Register::FifoControl, k_fifo_enable | k_fifo_clear_rx);
        }

        void flush_output()
        {
            write(Register::FifoControl, k_fifo_enable | k_fifo_clear_tx);
        }

        LineStatus line_status()
        {
            const uint8 raw = read(Register::LineStatus);
            observe_line_errors(raw);
            const uint8 new_errors =
                _observed_line_errors & ~_reported_line_errors;
            _reported_line_errors |= new_errors;
            return {
                .transmitter_empty =
                    (raw & k_line_transmitter_empty) != 0,
                .raw = raw,
                .new_errors = new_errors,
            };
        }

    private:
        enum class Register : uint32
        {
            ReceiverBuffer = 0,
            TransmitterHolding = 0,
            InterruptEnable = 1,
            FifoControl = 2,
            LineControl = 3,
            LineStatus = 5,
        };

        static constexpr uint8 k_interrupt_rx_ready = 1U << 0;
        static constexpr uint8 k_interrupt_transmit_empty = 1U << 1;
        static constexpr uint8 k_fifo_enable = 1U << 0;
        static constexpr uint8 k_fifo_clear_rx = 1U << 1;
        static constexpr uint8 k_fifo_clear_tx = 1U << 2;
        static constexpr uint8 k_word_length_8 = 3U << 0;
        static constexpr uint8 k_divisor_latch_access = 1U << 7;
        static constexpr uint8 k_line_data_ready = 1U << 0;
        static constexpr uint8 k_line_transmit_holding_empty = 1U << 5;
        static constexpr uint8 k_line_transmitter_empty = 1U << 6;
        static constexpr uint8 k_line_error_mask =
            (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4) | (1U << 7);

        void observe_line_errors(uint8 line_status)
        {
            _observed_line_errors |= line_status & k_line_error_mask;
        }

        void write(Register reg, uint8 value)
        {
            *reinterpret_cast<volatile uint8 *>(
                _base + static_cast<uint32>(reg)) = value;
        }

        uint8 read(Register reg) const
        {
            return *reinterpret_cast<volatile uint8 *>(
                _base + static_cast<uint32>(reg));
        }

        // 静态对象位于 BSS；initialize() 是唯一发布有效 MMIO 基址的入口。
        uint64 _base;
        uint8 _observed_line_errors;
        uint8 _reported_line_errors;
    };
} // namespace dev::serial

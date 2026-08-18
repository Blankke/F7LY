#pragma once

#include "char_device.hh"
#include "spinlock.hh"
#include "types.hh"

namespace dev
{
    void register_debug_uart(CharDevice *uart_port);

    /**
     * 控制台字符设备的通用管理层。
     *
     * 它只拥有软件发送队列、破坏性输入探测所需的单字节预读槽和并发锁。
     * MMIO、SBI、寄存器格式、波特率与板级 IRQ 均由 compile-time 选择的
     * platform::console_backend 实现。
     */
    class UartManager : public CharDevice
    {
    public:
        bool initialize();

        bool read_ready() override;
        bool write_ready() override;
        bool support_stream() override { return false; }

        int put_char_sync(u8 character) override;
        int put_char(u8 character) override;
        long put_chars_sync(const u8 *source, long nbytes) override;
        long put_chars_sync_crlf(const u8 *source, long nbytes) override;
        int get_char_sync(u8 *character) override;
        int get_char(u8 *character) override;
        int handle_intr() override;

        int get_input_buffer_size() override;
        int get_output_buffer_size() override;
        int flush_buffer(int queue) override;
        int get_line_status() override;

    private:
        static constexpr ulong k_output_buffer_size = 32;

        // 调用者必须持有 _lock；只把已经排队的字符提交给平台发送端。
        void start_transmit_locked();
        // 调用者必须持有 _write_transaction_lock；允许整次 write() 复用同一事务。
        int put_char_unserialized(u8 character);
        int put_char_sync_unserialized(u8 character);
        constexpr bool output_buffer_full() const
        {
            return _write_index - _read_index >= k_output_buffer_size;
        }

        SpinLock _lock;
        // 只串行化输出生产者，不参与 IRQ 排空，避免并发 write() 逐字符交错。
        SpinLock _write_transaction_lock;
        char _output_buffer[k_output_buffer_size];
        ulong _write_index;
        ulong _read_index;
        bool _pending_input_valid;
        u8 _pending_input;
    };

    // Console、中断分发和 /dev/stdin/out 必须共享这一份软件队列与锁。
    extern UartManager k_uart;
} // namespace dev

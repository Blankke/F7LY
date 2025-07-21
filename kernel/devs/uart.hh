#pragma once

#include "spinlock.hh"
#include "char_device.hh"
#include "types.hh"
namespace dev
{
	void register_debug_uart(CharDevice *uart_port);
	class UartManager : public CharDevice
	{
	private:
		uint64 _uart_base;
		SpinLock _lock;
		static const int _buf_size = 32;
		char _buf[_buf_size];
		ulong _wr_idx = 0;
		ulong _rd_idx = 0;
		char _read_buf[_buf_size];
		ulong _read_front = 0;
		ulong _read_tail = 0;

	public:
		UartManager() {}
		UartManager(uint64 reg_base) : _uart_base((u64)reg_base) {}
		virtual bool read_ready() override { return !_read_buffer_empty(); }
		virtual bool write_ready() override { return !_read_buffer_full(); }

		void init(uint64 u_addr);
		virtual int put_char_sync(u8 c);
		int put_char(u8 c);
		int get_char_sync(u8 *c);
		int get_char(u8 *c);
		void start();
		virtual bool support_stream() override { return false; }
		uint8 read_lsr();
		uint8 read_rhr();
		void write_thr(uint8 data);
		int handle_intr();
		
		// 实现 StreamDevice 的缓冲区查询接口
		virtual int get_input_buffer_size() override;
		virtual int get_output_buffer_size() override; 
		virtual int flush_buffer(int queue) override;
		virtual int get_line_status() override;

	private:
		//=========================中断相关==========================
		enum UartReg
		{
			RHR = 0,
			THR = 0,
			IER = 1,
			FCR = 2,
			ISR = 2,
			LCR = 3,
			LSR = 5,
		};
		enum UartIER
		{
			rx_en = 0x1 << 0,
			tx_en = 0x1 << 1,
		};
		enum UartFCR
		{
			enable = 0x1 << 0,
			rx_clear = 0x1 << 1,
			tx_clear = 0x1 << 2,
			clear = 0x3 << 1
		};
		enum UartLCR
		{
			use_8_bits = 0x3 << 0,
			access_baud = 0x1 << 7,
		};
		enum UartLSR
		{
			rx_ready = 0x1 << 0,
			tx_idle = 0x1 << 5,
		};
		enum UartBaud
		{
			low_8_bit = 0,
			high_8_bit = 1
		};
		struct regIER
		{
			uint8 data_ready : 1;
			uint8 thr_empty : 1;
			uint8 lsr_int : 1;
			uint8 modem_int : 1;
			uint8 _rev : 2;
			uint8 dma_rx : 1;
			uint8 dma_tx : 1;
		};
		static_assert(sizeof(regIER) == 1);
		struct regFCR
		{
			uint8 enable : 1;
			uint8 rx_fifo_reset : 1;
			uint8 tx_fifo_reset : 1;
			uint8 dma_mode : 1;
			uint8 dma_end_enable : 1;
			uint8 _rev : 1;
			uint8 rx_fifo_level : 2;
		};
		static_assert(sizeof(regFCR) == 1);
		struct regISR
		{
			uint8 intr_status : 1;
			uint8 intr_code : 3;
			uint8 dma_rx_end : 1;
			uint8 dma_tx_end : 1;
			uint8 fifo_enabled : 2;
		};
		static_assert(sizeof(regISR) == 1);
		struct regLCR
		{
			enum WordLen : uint8
			{
				bits_5 = 0,
				bits_6 = 1,
				bits_7 = 2,
				bits_8 = 3,
			};
			uint8 word_length : 2;
			uint8 stop_bits : 1;
			uint8 parity_enbale : 1;
			uint8 even_parity : 1;
			uint8 force_parity : 1;
			uint8 set_break : 1;
			uint8 divisor_access : 1;
		};
		static_assert(sizeof(regLCR) == 1);
		struct regMCR
		{
			uint8 dtr : 1;
			uint8 rts : 1;
			uint8 out1 : 1;
			uint8 out2 : 1;
			uint8 loop_back : 1;
			uint8 _rev : 3;
		};
		static_assert(sizeof(regMCR) == 1);
		struct regLSR
		{
			uint8 data_ready : 1;
			uint8 overrun_error : 1;
			uint8 parity_error : 1;
			uint8 framing_error : 1;
			uint8 break_intr : 1;
			uint8 thr_empty : 1;
			uint8 transmit_empty : 1;
			uint8 fifo_data_error : 1;
		};
		static_assert(sizeof(regLSR) == 1);
		struct regMSR
		{
			uint8 delta_cts : 1;
			uint8 delta_dsr : 1;
			uint8 trail_edge_ri : 1;
			uint8 delta_cd : 1;
			uint8 cts : 1;
			uint8 dst : 1;
			uint8 ri : 1;
			uint8 cd : 1;
		};
		static_assert(sizeof(regMSR) == 1);

		struct regPSD
		{
			uint8 precaler_division : 4;
			uint8 _rev : 4;
		};
		static_assert(sizeof(regPSD) == 1);

	private:
		void _write_reg(uint32 reg, uint8 data);
		uint8 _read_reg(uint32 reg);
		constexpr bool _read_buffer_full() { return _read_front == _read_tail + _buf_size; }
		constexpr bool _read_buffer_empty() { return _read_front == _read_tail; }
		constexpr void _read_buffer_put(char c)
		{
			_read_buf[_read_front % _buf_size] = c;
			_read_front++;
		}
		constexpr char _read_buffer_get()
		{
			_read_tail++;
			return _read_buf[(_read_tail - 1) % _buf_size];
		}
	};
	extern UartManager k_uart; // 全局的uart管理器
}
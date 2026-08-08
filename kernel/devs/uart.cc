#include "uart.hh"
#include "printer.hh"
#include "console.hh"
#include"device_manager.hh"
#ifdef RISCV
#include "hal/riscv/sbi.hh"
#endif

namespace dev
{
	UartManager k_uart;
	void register_debug_uart( CharDevice* uart_port )
	{
		k_devm.register_char_device( ( CharDevice * ) uart_port, DEFAULT_DEBUG_CONSOLE_NAME );
		k_stdin.redirect_stream( ( CharDevice * ) uart_port );
		k_stdout.redirect_stream( ( CharDevice * ) uart_port );
		k_stderr.redirect_stream( ( CharDevice * ) uart_port );
	}
	void UartManager::init(uint64 u_addr)
	{
		_uart_base = u_addr;

		_write_reg(UartReg::IER, 0x0);
#ifndef BOARD_LS2K1000
		// QEMU 的 UART 输入时钟固定，可使用历史 divisor=3。2K1000 的时钟来自
		// 板级时钟树，启动 DTB 又没有把时钟计算接入本驱动，因此实机保留
		// U-Boot 已配置好的 115200 divisor，避免用 QEMU 魔数破坏早期串口。
		_write_reg(UartReg::LCR, UartLCR::access_baud);
		_write_reg(UartBaud::low_8_bit, 0x03);
		_write_reg(UartBaud::high_8_bit, 0x00);
#endif
		_write_reg(UartReg::LCR, UartLCR::use_8_bits);
		_write_reg(UartReg::FCR, UartFCR::enable | UartFCR::clear);
		_write_reg(UartReg::IER, UartIER::rx_en);

		_lock.init("uart");
		_wr_idx = _rd_idx = 0;
		_pending_input_valid = false;
		_pending_input = 0;
	}

	bool UartManager::read_ready()
	{
		_lock.acquire();
		bool ready = _pending_input_valid;
#ifdef RISCV
		if (!ready)
		{
			const int ch = sbi_console_getchar();
			if (ch >= 0)
			{
				_pending_input = static_cast<u8>(ch);
				_pending_input_valid = true;
				ready = true;
			}
		}
#else
		ready = ready || (_read_reg(UartReg::LSR) & UartLSR::rx_ready) != 0;
#endif
		_lock.release();
		return ready;
	}

	bool UartManager::write_ready()
	{
		_lock.acquire();
		const bool ready = !_write_buffer_full();
		_lock.release();
		return ready;
	}

	int UartManager::put_char_sync(u8 c)
	{
		// 同步输出也进入同一发送队列，保证它不会越过已经排队的换行等字符。
		// 等待期间不持有 UART 锁，发送中断才能继续排空队列。
		if (put_char(c) < 0)
		{
			return -1;
		}
		for (;;)
		{
			_lock.acquire();
			start();
			const bool submitted = _wr_idx == _rd_idx;
			_lock.release();
			if (submitted)
			{
				return 0;
			}
			asm volatile("nop");
		}
	}

	int UartManager::put_char(u8 c)
	{
		if (k_printer.is_panic())
		{
			while (1)
				;
		}

		while (1)
		{
			_lock.acquire();
			if (_write_buffer_full())
			{
				// 尽量主动推进一次；若硬件仍忙，必须释放锁等待发送中断，
				// 不能拿着锁自旋，否则中断处理函数永远无法腾出空间。
				start();
				if (_write_buffer_full())
				{
					_lock.release();
					asm volatile("nop");
					continue;
				}
			}

			_buf[_wr_idx % _buf_size] = c;
			_wr_idx += 1;
			start();
			_lock.release();
			return 0;
		}
	}

	int UartManager::get_char_sync(u8* c)
	{
		if (c == nullptr)
			return -1;
		// 轮询调用非阻塞读取，每次失败都已经释放锁；这样 RX 中断不会因
		// 同步读取持锁等待硬件而发生自旋死锁。
		while (get_char(c) < 0)
		{
			asm volatile("nop");
		}
		return 0;
	}

	int UartManager::get_char(u8 *c)
	{
		if (c == nullptr)
			return -1;
		_lock.acquire();
		if (_pending_input_valid)
		{
			*c = _pending_input;
			_pending_input_valid = false;
			_lock.release();
			return 0;
		}
#ifdef RISCV
		const int ch = sbi_console_getchar();
		if (ch >= 0)
		{
			*c = static_cast<u8>(ch);
			_lock.release();
			return 0;
		}
#else
		if ((_read_reg(UartReg::LSR) & UartLSR::rx_ready) != 0)
		{
			*c = _read_reg(UartReg::RHR);
			_lock.release();
			return 0;
		}
#endif
		_lock.release();
		return -1;
	}

	void UartManager::start()
	{
		volatile regLSR *lsr = (volatile regLSR *)(_uart_base + LSR);
		volatile char *thr = (volatile char *)(_uart_base + THR);
		while (1)
		{
			if (_wr_idx == _rd_idx)
			{
				// transmit buffer is empty.
				_write_reg(UartReg::IER, UartIER::rx_en);
				return;
			}

			if (lsr->thr_empty == 0)
			{
				// the UART transmit holding register is full,
				// 打开发送空中断，硬件就绪后由 handle_intr() 继续排空队列。
				_write_reg(UartReg::IER, UartIER::rx_en | UartIER::tx_en);
				return;
			}

			char c = _buf[_rd_idx % _buf_size];
			_rd_idx += 1;

			// maybe uartputc() is waiting for space in the buffer.
			// TODO: wakeup_at( &_rd_idx );

			*thr = c;
		}
	}

	void UartManager::_write_reg(uint32 reg, uint8 data)
	{
		*(volatile unsigned char *)(_uart_base + reg) = data;
	}

	uint8 UartManager::_read_reg(uint32 reg)
	{
		return *(volatile unsigned char *)(_uart_base + reg);
	}

	uint8 UartManager::read_lsr()
	{
		return _read_reg(UartReg::LSR);
	}

	uint8 UartManager::read_rhr()
	{
		return _read_reg(UartReg::RHR);
	}

	void UartManager::write_thr(uint8 data)
	{
		_write_reg(UartReg::THR, data);
	}
	//=========================中断相关==========================
	int UartManager::handle_intr()
	{
		// 处理接收到的字符
		while (1)
		{
			_lock.acquire();
			if ((_read_reg(UartReg::LSR) & UartLSR::rx_ready) == 0)
			{
				start();
				_lock.release();
				break;
			}

			// 硬件 FIFO 只在 UART 锁内消费；释放锁后再进入 Console，避免
			// 与回显路径形成 UART -> Console -> UART 的锁嵌套。
			u8 c = _read_reg(UartReg::RHR);
			_lock.release();
			kConsole.console_intr(c);
		}
		return 0;
	}
	
	int UartManager::get_input_buffer_size()
	{
		_lock.acquire();
		// Console 行规程是唯一的软件 RX 队列；UART 这里只报告硬件/peek 字节。
		int bytes_available = _pending_input_valid ? 1 : 0;
#ifdef RISCV
		if (!_pending_input_valid)
		{
			const int ch = sbi_console_getchar();
			if (ch >= 0)
			{
				_pending_input = static_cast<u8>(ch);
				_pending_input_valid = true;
				bytes_available = 1;
			}
		}
#else
		bytes_available = bytes_available != 0 ||
		                          (_read_reg(UartReg::LSR) & UartLSR::rx_ready) != 0
		                      ? 1
		                      : 0;
#endif
		_lock.release();
		return bytes_available;
	}
	
	int UartManager::get_output_buffer_size()
	{
		_lock.acquire();
		// 计算写缓冲区中的字节数
		int bytes_buffered;
		bytes_buffered = static_cast<int>(_wr_idx - _rd_idx);
		_lock.release();
		return bytes_buffered;
	}
	
	int UartManager::flush_buffer(int queue)
	{
		_lock.acquire();
		
		switch(queue) {
			case 0: // TCIFLUSH - 清空输入缓冲区
				_pending_input_valid = false;
#ifdef RISCV
				while (sbi_console_getchar() >= 0) {}
#else
				while ((_read_reg(UartReg::LSR) & UartLSR::rx_ready) != 0)
					(void)_read_reg(UartReg::RHR);
#endif
				break;
			case 1: // TCOFLUSH - 清空输出缓冲区  
				_wr_idx = _rd_idx = 0;
				_write_reg(UartReg::IER, UartIER::rx_en);
				break;
			case 2: // TCIOFLUSH - 清空输入和输出缓冲区
				_pending_input_valid = false;
#ifdef RISCV
				while (sbi_console_getchar() >= 0) {}
#else
				while ((_read_reg(UartReg::LSR) & UartLSR::rx_ready) != 0)
					(void)_read_reg(UartReg::RHR);
#endif
				_wr_idx = _rd_idx = 0;
				_write_reg(UartReg::IER, UartIER::rx_en);
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
		uint8 lsr = read_lsr();
		int status = 0;
		
		// 检查发送器是否为空 (THR empty 和 TSR empty)
		if (lsr & UartLSR::tx_idle) {
			status |= 0x01; // TIOCSER_TEMT - 发送器物理为空
		}
		
		return status;
	}
};

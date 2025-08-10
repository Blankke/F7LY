#include "uart.hh"
#include "printer.hh"
#include "console.hh"
#include"device_manager.hh"
#include "sbi.hh"
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
		_write_reg(UartReg::LCR, UartLCR::access_baud);
		_write_reg(UartBaud::low_8_bit, 0x03);
		_write_reg(UartBaud::high_8_bit, 0x00);
		_write_reg(UartReg::LCR, UartLCR::use_8_bits);
		_write_reg(UartReg::FCR, UartFCR::enable | UartFCR::clear);
		_write_reg(UartReg::IER, UartIER::rx_en);

		_lock.init("uart");
		_wr_idx = _rd_idx = 0;
	}

	int UartManager::put_char_sync(u8 c)
	{
#ifdef QEMU
		if (k_printer.is_panic())
			while (1)
				;
		while ((_read_reg(UartReg::LSR) & UartLSR::tx_idle) == 0)
			;
		_write_reg(UartReg::THR, c);
#else
		sbi_console_putchar(c);
#endif
		return 0;
	}

	int UartManager::put_char(u8 c)
	{
#ifdef QEMU
		_lock.acquire();

		if (k_printer.is_panic())
		{
			while (1)
				;
		}

		while (1)
		{
			if (_wr_idx == _rd_idx + _buf_size)
			{
				// buffer is full.
				// wait for uartstart() to open up space in the buffer.
				// pm::k_pm.sleep( &tx_r, &_lock );
			}
			else
			{
				_buf[_wr_idx % _buf_size] = c;
				_wr_idx += 1;
				start();
				_lock.release();
				return 0;
			}
		}
#else
		sbi_console_putchar(c);
		return 0;
#endif
	}

	int UartManager::get_char_sync(u8* c)
	{
	#ifdef QEMU
		volatile regLSR *lsr = (volatile regLSR *)(_uart_base + LSR);
		while (lsr->data_ready == 0)
			;
		*c = _read_reg( UartReg::THR );
	#else
		int ch = sbi_console_getchar();
		if (ch == -1)
			return -1;
		*c = (u8)ch;
	#endif
		return 0;
	}

	int UartManager::get_char(u8 *c)
	{
	#ifdef QEMU
		if (_read_buffer_empty())
			return -1;
		else
		{
			*c = _read_buffer_get();
			return 0;
		}
	#else
		int ch = sbi_console_getchar();
		if (ch == -1)
			return -1;
		*c = (u8)ch;
		return 0;
	#endif
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
				return;
			}

			if (lsr->thr_empty == 0)
			{
				// the UART transmit holding register is full,
				// so we cannot give it another byte.
				// it will interrupt when it's ready for a new byte.
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
			volatile regLSR *lsr = (volatile regLSR *)(_uart_base + LSR);
			if (lsr->data_ready == 0)
				break;
			
			// 从硬件读取字符
			u8 c = _read_reg(UartReg::RHR);
			
			// 放入读缓冲区
			if (!_read_buffer_full()) {
				_read_buffer_put(c);
			}
			
			// 传递给控制台进行进一步处理
			kConsole.console_intr(c);
		}

		// send buffered characters.
		_lock.acquire();
		start();
		_lock.release();
		return 0;
	}
	
	int UartManager::get_input_buffer_size()
	{
		_lock.acquire();
		// 计算读缓冲区中的字节数
		int bytes_available;
		if (_read_front >= _read_tail) {
			bytes_available = _read_front - _read_tail;
		} else {
			// 循环缓冲区情况
			bytes_available = (_buf_size - _read_tail) + _read_front;
		}
		_lock.release();
		return bytes_available;
	}
	
	int UartManager::get_output_buffer_size()
	{
		_lock.acquire();
		// 计算写缓冲区中的字节数
		int bytes_buffered;
		if (_wr_idx >= _rd_idx) {
			bytes_buffered = _wr_idx - _rd_idx;
		} else {
			// 循环缓冲区情况
			bytes_buffered = (_buf_size - _rd_idx) + _wr_idx;
		}
		_lock.release();
		return bytes_buffered;
	}
	
	int UartManager::flush_buffer(int queue)
	{
		_lock.acquire();
		
		switch(queue) {
			case 0: // TCIFLUSH - 清空输入缓冲区
				_read_front = _read_tail = 0;
				break;
			case 1: // TCOFLUSH - 清空输出缓冲区  
				_wr_idx = _rd_idx = 0;
				break;
			case 2: // TCIOFLUSH - 清空输入和输出缓冲区
				_read_front = _read_tail = 0;
				_wr_idx = _rd_idx = 0;
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
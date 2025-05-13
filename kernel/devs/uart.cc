#include "uart.hh"
#include "printer.hh"
#include "console.hh"
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

void UartManager::putc_sync(int c)
{

	if (k_printer.is_panic())
		while (1)
			;
	while ((_read_reg(UartReg::LSR) & UartLSR::tx_idle) == 0)
		;
	_write_reg(UartReg::THR, c);
}

void UartManager::putc(int c)
{
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
			return;
		}
	}
}

int UartManager::getc_sync()
{
	volatile regLSR *lsr = (volatile regLSR *)(_uart_base + LSR);
	while (lsr->data_ready == 0)
		;

	return _read_reg(UartReg::THR);
}

int UartManager::getc()
{
	if (_read_buffer_empty())
		return -1;
	else
	{
		return _read_buffer_get();
	}
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
int UartManager::uart_intr()
{
	while (1)
	{
		int c = getc();
		if (c == -1)
			break;
		return c;
		kConsole.console_intr(c);
	}

	// send buffered characters.
	_lock.acquire();
	start();
	_lock.release();
	return 0;
}
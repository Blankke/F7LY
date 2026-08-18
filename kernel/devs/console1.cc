//
// Copied from Li Shuang ( pseudonym ) on 2024-07-25 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "console1.hh"
#include "console.hh"
#include "console_termios.hh"
#include "printer.hh"
#include "scheduler.hh"
#include "signal.hh"
#include "proc_manager.hh"
#include "tm/time.hh"
#include "tm/timer_interface.hh"

namespace dev
{
// >>>> Console - STDIN

	long ConsoleStdin::write( void *, long )
	{
		printfYellow( "try to write stdin device" );
		return -1;
	}

	namespace
	{
		long write_console_output(CharDevice *stream, const void *source, long nbytes)
		{
			if (stream == nullptr || source == nullptr || nbytes <= 0)
			{
				return 0;
			}

			const auto *bytes = reinterpret_cast<const u8 *>(source);
			if (!k_console_termios.map_output_newline())
			{
				return stream->put_chars_sync(bytes, nbytes);
			}

			// 默认 OPOST|ONLCR：设备负责在同一批量事务内将 LF 映射为 CRLF；
			// 返回值仍按用户输入字节计数，插入的 CR 不属于 write(2) 消费长度。
			return stream->put_chars_sync_crlf(bytes, nbytes);
		}

		uint64 console_vtime_to_ticks(unsigned char deciseconds)
		{
			if (deciseconds == 0)
			{
				return 0;
			}
			const uint64 timeout_us = static_cast<uint64>(deciseconds) * 100 * 1000;
			return (timeout_us + tmm::tick_period_us - 1) / tmm::tick_period_us;
		}

		bool console_deadline_reached(uint64 now, uint64 deadline)
		{
			return now >= deadline;
		}
	}

	long ConsoleStdin::read_available( void * dst, long nbytes )
	{
		long copied = kConsole.console_read_kernel(dst, nbytes);
		if (copied > 0)
		{
			return copied;
		}

		// 某些平台/后端上串口 RX 中断可能晚到甚至丢掉一次唤醒；
		// 如果硬件自己已经声明“有字节可读”，就主动拉一个字节走
		// console 行规程，再尝试从行规程缓冲中取数。
		if (_stream != nullptr && _stream->read_ready())
		{
			u8 c = 0;
			if (_stream->get_char(&c) == 0)
			{
				kConsole.console_intr(c);
				copied = kConsole.console_read_kernel(dst, nbytes);
				if (copied > 0)
				{
					return copied;
				}
			}
			else
			{
				// read_ready 与 get_char 之间可能刚好由 RX 中断取走字符；
				// 此时字符已经进入 Console 队列，只需重查，不能阻塞等下一个字符。
				copied = kConsole.console_read_kernel(dst, nbytes);
				if (copied > 0)
				{
					return copied;
				}
			}
		}

		return 0;
	}

	bool ConsoleStdin::has_pending_signal()
	{
		proc::Pcb * cur = proc::k_pm.get_cur_pcb();
		return cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending( cur );
	}

	long ConsoleStdin::read( void * dst, long nbytes, bool nonblocking )
	{
		if ( dst == nullptr || nbytes <= 0 )
		{
			return 0;
		}

		ConsoleReadSettings settings = k_console_termios.read_settings();
		char *out = reinterpret_cast<char *>(dst);
		long total = 0;
		const uint64 timeout_ticks = console_vtime_to_ticks(settings.timeout_deciseconds);
		uint64 deadline = tmm::get_ticks() + timeout_ticks;
		long target_bytes = settings.min_bytes;
		if (target_bytes <= 0)
		{
			target_bytes = 1;
		}
		if (target_bytes > nbytes)
		{
			target_bytes = nbytes;
		}

		// stdin 走 console 行规程缓冲：canonical 模式只在整行提交后返回；
		// non-canonical 模式额外遵循 VMIN/VTIME，并让 O_NONBLOCK 空读返回 EAGAIN。
		while ( true )
		{
			long copied = read_available(out + total, nbytes - total);
			if (copied > 0)
			{
				total += copied;
				if (settings.canonical)
				{
					return total;
				}
				if (nonblocking || settings.min_bytes == 0 || total >= target_bytes || total == nbytes)
				{
					return total;
				}
				if (timeout_ticks > 0)
				{
					// MIN>0/TIME>0 使用 inter-byte timer；每次读到新字节后重置。
					deadline = tmm::get_ticks() + timeout_ticks;
				}
			}

			if (nonblocking)
			{
				return total > 0 ? total : -EAGAIN;
			}

			if (!settings.canonical)
			{
				if (settings.min_bytes == 0)
				{
					if (timeout_ticks == 0)
					{
						return total;
					}
					if (console_deadline_reached(tmm::get_ticks(), deadline))
					{
						return total;
					}
				}
				else if (timeout_ticks > 0 && total > 0 &&
				         console_deadline_reached(tmm::get_ticks(), deadline))
				{
					return total;
				}
			}

			if ( has_pending_signal() )
			{
				return total > 0 ? total : -EINTR;
			}

			proc::k_scheduler.yield();
		}
	}

		int ConsoleStdin::get_input_buffer_size()
		{
			int buffered = kConsole.buffered_input_size();
			if (buffered > 0)
			{
				return buffered;
			}
			if (_stream != nullptr && _stream->read_ready())
			{
				return 1;
			}
			return 0;
		}

	int ConsoleStdin::flush_buffer(int queue)
	{
		if (queue == 0 || queue == 2)
		{
			kConsole.flush_input();
		}
		if (queue == 1 || queue == 2)
		{
			if (_stream != nullptr)
			{
				return _stream->flush_buffer(queue == 2 ? 1 : queue);
			}
		}
		return 0;
	}

// <<<< Console - STDIN

// >>>> Console - STDOUT

	long ConsoleStdout::write( void * src, long nbytes )
	{
		if ( _stream == nullptr )
		{
			printfYellow( "未绑定流" );
			return 0;
		}
		return write_console_output(_stream, src, nbytes);
	}

	long ConsoleStdout::read( void *, long, bool )
	{
		printfYellow( "try to read stdout device" );
		return -1;
	}

// <<<< Console - STDOUT

// >>>> Console - STDERR

	long ConsoleStderr::write( void * src, long nbytes )
	{
		if ( _stream == nullptr )
		{
			printfYellow( "stream not be bound" );
			return 0;
		}
		return write_console_output(_stream, src, nbytes);
	}

	long ConsoleStderr::read( void *, long, bool )
	{
		printfYellow( "try to read stdout device" );
		return -1;
	}

// <<<< Console - STDERR

} 

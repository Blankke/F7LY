//
// Copied from Li Shuang ( pseudonym ) on 2024-07-25 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "console1.hh"
#include "console.hh"
#include "printer.hh"
#include "scheduler.hh"
#include "signal.hh"
#include "proc_manager.hh"

namespace dev
{
// >>>> Console - STDIN

	long ConsoleStdin::write( void *, long )
	{
		printfYellow( "try to write stdin device" );
		return -1;
	}

	long ConsoleStdin::read( void * dst, long nbytes )
	{
		if ( dst == nullptr || nbytes <= 0 )
		{
			return 0;
		}

		proc::Pcb * cur = proc::k_pm.get_cur_pcb();

		// stdin 走 console 的行规程缓冲，而不是直接读 UART 原始字节。
		// 这样 canonical 模式下回车才能真正提交一整行，raw 模式下也能靠
		// console_intr() 的 w_idx 推进按字节唤醒用户态。
		while ( true )
		{
			long copied = kConsole.console_read_kernel(dst, nbytes);
			if (copied > 0)
			{
				return copied;
			}

			// 某些平台/后端上串口 RX 中断可能晚到甚至丢掉一次唤醒；
			// 如果硬件自己已经声明“有字节可读”，就主动拉一个字节走
			// console 行规程，保证 poll/read 不会永远卡在空缓冲上。
			if (_stream != nullptr && _stream->read_ready())
			{
				u8 c = 0;
				if (_stream->get_char(&c) == 0 || _stream->get_char_sync(&c) == 0)
				{
					kConsole.console_intr(c);
					continue;
				}
			}

			if ( cur != nullptr && proc::ipc::signal::has_unmasked_signal_pending( cur ) )
			{
				return -EINTR;
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
		u8 * ptr = ( u8 * ) src;
		for ( long i = 0; i < nbytes; i++, ptr++ )
			if ( _stream->put_char_sync( *ptr ) < 0 )
				return i;
		return nbytes;
	}

	long ConsoleStdout::read( void *, long )
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
		u8 * ptr = ( u8 * ) src;
		for ( long i = 0; i < nbytes; i++, ptr++ )
			if ( _stream->put_char_sync( *ptr ) < 0 )
				return i;
		return nbytes;
	}

	long ConsoleStderr::read( void *, long )
	{
		printfYellow( "try to read stdout device" );
		return -1;
	}

// <<<< Console - STDERR

} 

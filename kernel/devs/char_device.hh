//
// Copy from Li Shuang ( pseudonym ) on 2024-07-15 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once

#include "virtual_device.hh"
#include "types.hh"
namespace dev
{
	class CharDevice : public VirtualDevice
	{
	public:
		CharDevice() = default;
		virtual DeviceType type() override { return DeviceType::dev_char; }
		virtual bool support_stream() = 0;
		virtual int get_char_sync( u8 *c ) = 0;
		virtual int get_char( u8 *c ) = 0;
		virtual int put_char_sync( u8 c ) = 0;
		virtual int put_char( u8 c ) = 0;
		// 默认逐字节写入；需要保证一次 write() 不被其它输出穿插的设备可重载。
		virtual long put_chars_sync( const u8 *src, long n_bytes )
		{
			if ( src == nullptr || n_bytes <= 0 )
				return 0;
			for ( long i = 0; i < n_bytes; ++i )
				if ( put_char_sync( src[i] ) < 0 )
					return i;
			return n_bytes;
		}
		// TTY 的 OPOST|ONLCR 输出路径：把输入 LF 映射为 CRLF，返回值仍按
		// 输入字节计数。具体设备可重载，以保证一次 write() 不被并发输出穿插。
		virtual long put_chars_sync_crlf( const u8 *src, long n_bytes )
		{
			if ( src == nullptr || n_bytes <= 0 )
				return 0;
			for ( long i = 0; i < n_bytes; ++i )
			{
				if ( src[i] == '\n' && put_char_sync( '\r' ) < 0 )
					return i;
				if ( put_char_sync( src[i] ) < 0 )
					return i;
			}
			return n_bytes;
		}
		virtual int handle_intr() = 0;
		
		// 缓冲区管理接口 - 提供默认实现
		virtual int get_input_buffer_size() { return 0; }
		virtual int get_output_buffer_size() { return 0; }  
		virtual int flush_buffer(int queue) { return 0; }
		virtual int get_line_status() { return 0x01; } // 默认返回 TIOCSER_TEMT
	};

} // namespace dev

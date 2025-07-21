//
// Copy from Li Shuang ( pseudonym ) on 2024-07-25 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once

#include "char_device.hh"

namespace dev
{
	class StreamDevice : public CharDevice
	{
	protected:
		CharDevice * _stream = nullptr;

	public:
		StreamDevice() = default;
		virtual ~StreamDevice() = default;
		virtual bool support_stream() override { return true; }
		virtual int get_char_sync( u8 *c ) override;
		virtual int get_char( u8 *c ) override;
		virtual int put_char_sync( u8 c ) override;
		virtual int put_char( u8 c ) override;
		virtual int handle_intr() override;

		virtual long write( void * src, long n_bytes ) = 0;
		virtual long read( void * dst, long n_bytes ) = 0;
		
		/// @brief 获取输入缓冲区中的字节数
		/// @return 输入缓冲区字节数，失败返回-1
		virtual int get_input_buffer_size() { return 0; } // 默认实现返回0
		
		/// @brief 获取输出缓冲区中的字节数  
		/// @return 输出缓冲区字节数，失败返回-1
		virtual int get_output_buffer_size() { return 0; } // 默认实现返回0
		
		/// @brief 刷新指定的缓冲区
		/// @param queue 缓冲区类型 (TCIFLUSH/TCOFLUSH/TCIOFLUSH)
		/// @return 成功返回0，失败返回-1
		virtual int flush_buffer(int queue) { return 0; } // 默认实现返回成功
		
		/// @brief 获取线路状态寄存器
		/// @return LSR状态，失败返回-1
		virtual int get_line_status() { return 0x01; } // 默认返回 TIOCSER_TEMT

	public:
		int redirect_stream( CharDevice * dev );
	};
} // namespace dev

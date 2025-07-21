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
		virtual int handle_intr() = 0;
		
		// 缓冲区管理接口 - 提供默认实现
		virtual int get_input_buffer_size() { return 0; }
		virtual int get_output_buffer_size() { return 0; }  
		virtual int flush_buffer(int queue) { return 0; }
		virtual int get_line_status() { return 0x01; } // 默认返回 TIOCSER_TEMT
	};

} // namespace dev

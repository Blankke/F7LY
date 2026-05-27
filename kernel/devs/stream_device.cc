//
// Copy from Li Shuang ( pseudonym ) on 2024-07-25 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "stream_device.hh"
#include "printer.hh"

namespace dev
{
	int StreamDevice::redirect_stream( CharDevice * dev )
	{
		if ( dev->type() != dev_char )
		{
			printfYellow( "try to bind stream with a device that's not char-dev" );
			return -1;
		}
		_stream = dev;
		return 0;
	}

	int StreamDevice::get_char_sync( u8 *c )
	{
		if ( _stream != nullptr )
			return _stream->get_char_sync( c );
		return 0;
	}
	int StreamDevice::get_char( u8 *c )
	{
		if ( _stream != nullptr )
			return _stream->get_char( c );
		return 0;
	}
	int StreamDevice::put_char_sync( u8 c )
	{
		if ( _stream != nullptr )
			return _stream->put_char_sync( c );
		return 0;
	}
	int StreamDevice::put_char( u8 c )
	{
		if ( _stream != nullptr )
			return _stream->put_char( c );
		return 0;
	}
	int StreamDevice::handle_intr()
	{
		if ( _stream != nullptr )
			return _stream->handle_intr();
		return 0;
	}

	int StreamDevice::get_input_buffer_size()
	{
		if ( _stream != nullptr )
			return _stream->get_input_buffer_size();
		return 0;
	}

	int StreamDevice::get_output_buffer_size()
	{
		if ( _stream != nullptr )
			return _stream->get_output_buffer_size();
		return 0;
	}

	int StreamDevice::flush_buffer(int queue)
	{
		if ( _stream != nullptr )
			return _stream->flush_buffer(queue);
		return 0;
	}

	int StreamDevice::get_line_status()
	{
		if ( _stream != nullptr )
			return _stream->get_line_status();
		return 0x01;
	}

} // namespace dev

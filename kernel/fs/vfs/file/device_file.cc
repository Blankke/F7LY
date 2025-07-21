#include "fs/vfs/file/device_file.hh"
#include "common.hh"
#include "device_manager.hh"
#include "stream_device.hh"

#include <termios.h>

namespace fs
{

	long device_file::read( uint64 buf, size_t len, long off, bool upgrade )
	{
		printfMagenta("[file] it is a device file\n");
		int ret;

		if ( _attrs.u_read != 1 )
		{
			printfRed( "device_file:: not allowed to read! " );
			return -1;
		}

		// Inode *node = _dentry->getNode();
		
		// if ( node == nullptr )
		// {
		// 	printfRed( "device_file:: null inode for dentry %s", _dentry->rName().c_str() );
		// 	return -1;
		// }

		// ret = node->nodeRead( buf, off, len );

		panic("device_file::read: not implemented yet");
		ret = 0;

		/// @note 对于流式设备而言，没有文件偏移的概念
		// if ( ret >= 0 && upgrade )  // 这里 没有写sdev从指定位置读取的函数
		// 	off += ret;
		return ret;

	}

	long device_file::write( uint64 buf, size_t len, long off, bool upgrade )
	{
		// panic("stdin 的 write 要转发到uart上。今天不想修了。7.13。");
		if (_attrs.u_write != 1)
		{
			printfRed("device_file:: not allowed to write! ");
			return -1;
		}
		int ret;

		dev::StreamDevice *sdev = (dev::StreamDevice *)dev::k_devm.get_device(_dev_num);
		if (sdev == nullptr)
		{
			printfRed("file write: null device for device number %d", _dev_num);
			return -1;
		}

		if (sdev->type() != dev::dev_char)
		{
			printfRed("file write: device %d is not a char-dev", _dev_num);
			return -1;
		}

		if (!sdev->support_stream())
		{
			printfRed("file write: device %d is not a stream-dev", _dev_num);
			return -1;
		}

		ret = sdev->write((void*)buf, len);

		return ret;
	}

	bool device_file::read_ready()
	{
		panic("device_file::read_ready: not implemented yet");
		// if( _dev == nullptr )
		// {
		// 	dev_t dev = _dentry->getNode()->rDev();
		// 	_dev = ( dev::StreamDevice * ) dev::k_devm.get_device( dev );
			
		// 	if( _dev == nullptr )
		// 	{
		// 		printfRed( "device_file::read_ready: null device for device number %d", dev );
		// 		return false;
		// 	}

		// 	if( _dev->type() != dev::dev_char )
		// 	{
		// 		printfRed( "device_file::read_ready: device %d is not a char-dev", dev );
		// 		return false;
		// 	}

		// 	if( !_dev->support_stream() )
		// 	{
		// 		printfRed( "device_file::read_ready: device %d is not a stream-dev", dev );
		// 		return false;
		// 	}
		// }

		// return _dev->read_ready();
	}

	bool device_file::write_ready()
	{
		panic("device_file::write_ready: not implemented yet");
		// if( _dev == nullptr )
		// {
		// 	dev_t dev = _dentry->getNode()->rDev();
		// 	_dev = ( dev::StreamDevice * ) dev::k_devm.get_device( dev );
			
		// 	if( _dev == nullptr )
		// 	{
		// 		printfRed( "device_file::read_ready: null device for device number %d", dev );
		// 		return false;
		// 	}

		// 	if( _dev->type() != dev::dev_char )
		// 	{
		// 		printfRed( "device_file::read_ready: device %d is not a char-dev", dev );
		// 		return false;
		// 	}

		// 	if( !_dev->support_stream() )
		// 	{
		// 		printfRed( "device_file::read_ready: device %d is not a stream-dev", dev );
		// 		return false;
		// 	}
		// }
		
		// return _dev->write_ready();
	}

	int device_file::tcgetattr( termios * ts )
	{
		// ts->c_ispeed = ts->c_ospeed = B115200;

		return 0;
	}
	
	int device_file::get_input_buffer_bytes()
	{
		dev::CharDevice *cdev = (dev::CharDevice *)dev::k_devm.get_device(_dev_num);
		if (cdev == nullptr)
		{
			return -1;
		}
		
		if (cdev->type() != dev::dev_char)
		{
			return -1;
		}
		
		return cdev->get_input_buffer_size();
	}
	
	int device_file::get_output_buffer_bytes()  
	{
		dev::CharDevice *cdev = (dev::CharDevice *)dev::k_devm.get_device(_dev_num);
		if (cdev == nullptr)
		{
			return -1;
		}
		
		if (cdev->type() != dev::dev_char)
		{
			return -1;
		}
		
		return cdev->get_output_buffer_size();
	}
	
	int device_file::flush_buffer(int queue)
	{
		dev::CharDevice *cdev = (dev::CharDevice *)dev::k_devm.get_device(_dev_num);
		if (cdev == nullptr)
		{
			return -1;
		}
		
		if (cdev->type() != dev::dev_char)
		{
			return -1;
		}
		
		return cdev->flush_buffer(queue);
	}
	
	int device_file::get_line_status()
	{
		dev::CharDevice *cdev = (dev::CharDevice *)dev::k_devm.get_device(_dev_num);
		if (cdev == nullptr)
		{
			return -1;
		}
		
		if (cdev->type() != dev::dev_char)
		{
			return -1;
		}
		
		return cdev->get_line_status();
	}
}
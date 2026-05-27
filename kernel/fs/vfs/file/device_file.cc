#include "fs/vfs/file/device_file.hh"
#include "common.hh"
#include "device_manager.hh"
#include "stream_device.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "proc_manager.hh"

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

		// 获取设备对象
		dev::VirtualDevice *device = dev::k_devm.get_device(_dev_num);
		if (device == nullptr) {
			printfRed("device_file::read: device not found for device number %d\n", _dev_num);
			return -1;
		}

		// 检查是否是字符设备
		if (device->type() != dev::dev_char) {
			printfRed("device_file::read: device is not a character device\n");
			return -1;
		}

		// 转换为StreamDevice来调用read方法
		dev::StreamDevice *stream_device = static_cast<dev::StreamDevice*>(device);
		if (stream_device == nullptr) {
			printfRed("device_file::read: device is not a stream device\n");
			return -1;
		}

		// VFS 层的 read() 约定和普通文件一致：buf 指向的是内核临时缓冲区，
		// 最终再由 syscall 层统一 copy_out 到用户地址。设备文件这里不能再
		// 把它误当成用户地址二次 copy_out，否则 stdin 一有数据就会返回 -EFAULT。
		ret = stream_device->read(reinterpret_cast<void *>(buf), len);
		if (ret < 0) {
			printfRed("device_file::read: device read failed with error %d\n", ret);
			return ret;
		}

		/// @note 对于设备文件，根据upgrade参数决定是否更新文件指针
		if ( ret >= 0 && upgrade )
			_file_ptr += ret;
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

		/// @note 根据upgrade参数决定是否更新文件指针
		if ( ret >= 0 && upgrade )
			_file_ptr += ret;

		return ret;
	}

	bool device_file::read_ready()
	{
		// select/poll/epoll 会通过 read_ready 查询设备当前是否可读。
		// 这里不能再用 panic 把整条回归链路掐断；设备暂时不可用时返回 false，
		// 由上层按照 Linux 的阻塞/超时语义继续处理。
		dev::VirtualDevice *device = dev::k_devm.get_device(_dev_num);
		if (device == nullptr)
		{
			printfRed("device_file::read_ready: device not found for device number %d\n", _dev_num);
			return false;
		}

		return device->read_ready();
	}

	bool device_file::write_ready()
	{
		dev::VirtualDevice *device = dev::k_devm.get_device(_dev_num);
		if (device == nullptr)
		{
			printfRed("device_file::write_ready: device not found for device number %d\n", _dev_num);
			return false;
		}

		return device->write_ready();
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

	off_t device_file::lseek( off_t offset, int whence )
	{
		off_t new_offset;
		
		switch (whence)
		{
			case SEEK_SET:
				new_offset = offset;
				break;
			case SEEK_CUR:
				new_offset = _file_ptr + offset;
				break;
			case SEEK_END:
				// 对于设备文件，SEEK_END 的行为可能因设备类型而异
				// 这里假设设备文件大小为0（流式设备）
				new_offset = 0 + offset;
				break;
			default:
				printfRed("device_file::lseek: invalid whence %d\n", whence);
				return -EINVAL;
		}
		
		// 检查新偏移量是否有效（不能为负数）
		if (new_offset < 0)
		{
			printfRed("device_file::lseek: invalid offset %ld\n", new_offset);
			return -EINVAL;
		}
		
		// 更新文件指针
		_file_ptr = new_offset;
		return new_offset;
	}
}

#include "fs/vfs/file/file.hh"
#include "printer.hh"
struct termios;

namespace dev
{
	class StreamDevice;
} // namespace dev


namespace fs
{
	class dentry;
	class device_file : public file
	{
	private:
		int off = 0;
		// dev::StreamDevice * _dev = nullptr;
		// dentry * _dentry = nullptr;
		eastl::string path_name;
		uint _dev_num;

	public:
		device_file() = default;
		/// @brief 设备文件的构造函数，初始化文件属性、设备号和目录项指针，并增加引用计数。
		/// @param attrs 文件属性，用于初始化基类 file。
		/// @param dev 设备号（当前参数已废弃，实际设备号应从 node 中获取）。
		/// @param den 指向目录项（dentry）的指针，用于标识该设备文件在文件系统中的位置。
		// device_file( FileAttrs attrs, uint dev, dentry *den ) : file( attrs), _dentry( den ) { dup(); };  // 这里 device 的 dev已经没有用了，应该去node里面找
		/// @brief 设备文件构造函数，初始化设备文件对象并增加引用计数。
		/// @param dev 设备编号，用于标识具体的设备。
		/// @param den 指向目录项（dentry）的指针，表示该设备文件在文件系统中的位置。
		// device_file( uint dev, dentry *den ) : device_file( FileAttrs( FileTypes::FT_DEVICE, 0777 ), dev, den ) { dup();};
		device_file(FileAttrs attrs, eastl::string path) : file(attrs), path_name(path) { dup(); };
		device_file(FileAttrs attrs, eastl::string path, uint dev_num) : file(attrs), path_name(path), _dev_num(dev_num) { dup(); };
		~device_file() = default;

		/// @brief 从设备文件中读取数据到指定缓冲区。
		/// @param buf 目标缓冲区的地址，用于存放读取到的数据。
		/// @param len 需要读取的数据长度（字节数）。
		/// @param off 从文件的指定偏移量开始读取数据。
		/// @param upgrade 这里没用上
		/// @return 实际读取的字节数，若发生错误则返回-1。
		long read( uint64 buf, size_t len, long off, bool upgrade = true ) override;

		/// @brief 向设备文件写入数据。
		/// @param buf 要写入的数据缓冲区的起始地址（64位无符号整数）。
		/// @param len 要写入的数据长度（字节数）。
		/// @param off 写入操作的起始偏移量。
		/// @param upgrade 这里没用上
		/// @return 实际写入的字节数，若发生错误则返回负值表示错误代码。
		long write( uint64 buf, size_t len, long off, bool upgrade = true ) override;

		/// @brief 判断设备是否已准备好进行读取操作。
		virtual bool read_ready() override;
		/// @brief 判断设备是否已准备好进行写入操作。
		virtual bool write_ready() override;
		
		/// @brief 移动文件指针到指定位置。当前 streamdevice 不支持 lseek 操作，调用此函数会返回错误。
		/// @param offset 以字节为单位的偏移量，用于指定新的文件指针位置。
		/// @param whence 指定偏移量的起始位置，可以是 SEEK_SET（文件开头）、SEEK_CUR（当前位置）或 SEEK_END（文件末尾）。
		/// @return 返回新的文件指针位置（成功时），或返回负值（如 -EINVAL）表示不支持该操作。
		virtual off_t lseek( off_t offset, int whence ) override { printfRed( "streamdevice not support lseek currently!" );return -EINVAL; };
		int tcgetattr( termios * ts );
	};
}
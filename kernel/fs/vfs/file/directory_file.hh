#include "fs/vfs/file/file.hh"
#include "fs/vfs/vfs_ext4_ext.hh"

namespace mem
{
	class UserspaceStream;
}

namespace fs
{
	class directory_file : public file
	{
	public:
		dentry *_den;

        public : directory_file() = default;
        directory_file(FileAttrs attrs, eastl::string path) : file(attrs, path)
		{
			dup();
			new(&_stat) Kstat(attrs.filetype);
		}
		~directory_file() = default;

		/// @brief 从目录文件中读取目录项到指定缓冲区。
		/// @param buf 目标缓冲区的地址，用于存放读取到的目录项数据。
		/// @param len 需要读取的数据长度（字节数）。
		/// @param off off=-1 表示不指定偏移使用文件内部偏移量
		/// @param upgrade 如果 upgrade 为 true，文件指针自动后移。
		/// @return 实际读取的字节数，若发生错误则返回负值表示错误码。
		virtual long read(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;

		/// @brief 目录文件不支持写操作。
		/// @param buf 要写入的数据缓冲区的地址（以 uint64 表示）。
		/// @param len 要写入的数据长度（以字节为单位）。
		/// @param off off=-1 表示不指定偏移使用文件内部偏移量
		/// @param upgrade 如果 upgrade 为 true，写完后文件指针自动后移。
		/// @return 总是返回 -1，表示不支持写操作。
		virtual long write(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;
		
		virtual bool read_ready() override;
		virtual bool write_ready() override;
		virtual off_t lseek(off_t offset, int whence) override;

		using ubuf = mem::UserspaceStream;
		
		/// @brief 读取目录中的子目录项。
		/// @param dst 目标用户空间流对象。
		/// @return 实际读取的字节数。
		size_t read_sub_dir(ubuf &dst);
		
		/// @brief 使用getdents64系统调用读取目录项。
		/// @param dirp 指向linux_dirent64结构的缓冲区。
		/// @param count 缓冲区大小。
		/// @return 实际读取的字节数，失败返回负数。
		long getdents64(struct linux_dirent64 *dirp, int count);
		
		void setAppend();
		dentry *getDentry() { return _den; }
	};
}

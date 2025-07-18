#include "fs/vfs/file/directory_file.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4.hh"
#include "fs/vfs/vfs_ext4_ext.hh"
#include "mem/userspace_stream.hh"
#include "fs/vfs/vfs_utils.hh"

namespace fs
{
	long directory_file::read(uint64 buf, size_t len, long off, bool upgrade)
	{
        // panic("directory_file::read: not implemented yet");
		// 目录文件的read操作主要是读取目录项
		if (_attrs.u_read != 1)
		{
			printfRed("directory_file:: not allowed to read! ");
			return -1;
		}
		
		if (_attrs.filetype != FileTypes::FT_DIRECT)
		{
			printfRed("directory_file:: not a directory file! ");
			return -1;
		}
		
		// 对于目录文件，通常使用getdents64来读取目录项
		// 这里假设buf指向linux_dirent64结构
		struct linux_dirent64 *dirp = (struct linux_dirent64 *)buf;
		return getdents64(dirp, len);
	}

	long directory_file::write(uint64 buf, size_t len, long off, bool upgrade)
	{
		// 目录文件不支持直接写操作
		printfRed("directory_file:: write operation not supported for directories! ");
		return -1;
	}

	bool directory_file::read_ready()
	{
		// 只有目录类型的文件才能进行目录读取操作
		return (_attrs.filetype == FileTypes::FT_DIRECT);
	}

	bool directory_file::write_ready()
	{
		// 目录文件不支持写操作
		return false;
	}

	size_t directory_file::read_sub_dir(ubuf &dst)
	{
		// 读取子目录项的实现
		// 这里需要具体的实现来遍历目录项
		panic("directory_file::read_sub_dir: not implemented yet");
		size_t rlen = 0; // Placeholder for actual read length
		return rlen;
	}

	long directory_file::getdents64(struct linux_dirent64 *dirp, int count)
	{
        // panic("directory_file::getdents64: not implemented yet");
		// 使用ext4文件系统的getdents功能
		// 这里需要一个临时的file结构来调用vfs_ext_getdents
		// 实际实现需要根据具体的文件系统接口调整
		
		// 临时实现，需要根据实际的文件系统接口进行调整
		// struct file temp_file;
		// 设置temp_file的相关字段...
		
		// 调用底层的getdents实现
		int result = vfs_getdents(this, dirp, count);
		
		if (result > 0) {
			// 更新文件指针
			_file_ptr += result;
		}
		
		return result;
	}

	off_t directory_file::lseek(off_t offset, int whence)
	{
		// 目录文件的seek操作
		off_t new_off;
		switch (whence)
		{
		case SEEK_SET:
			if (offset < 0)
				return -EINVAL;
			_file_ptr = offset;
			break;
		case SEEK_CUR:
			new_off = _file_ptr + offset;
			if (new_off < 0)
				return -EINVAL;
			_file_ptr = new_off;
			break;
		case SEEK_END:
			// 对于目录，SEEK_END通常设置到目录末尾
			// 这里简单设置为0，实际实现可能需要获取目录大小
			_file_ptr = 0;
			break;
		default:
			printfRed("directory_file::lseek: invalid whence %d", whence);
			return -EINVAL;
		}
		panic("不知道哪里有ext4_dir_lseek, 下面用fseek感觉不对");
		// int seek_status = ext4_fseek(&lwext4_file_struct, _file_ptr, SEEK_SET);
		// if (seek_status != EOK)
		// {
		// 	printfRed("normal_file::read: ext4_fseek failed with status %d", seek_status);
		// 	return -1;
		// }
		return _file_ptr;
	}

	void directory_file::setAppend()
	{
		// 对于目录文件，append操作通常没有意义
		// 但为了保持接口一致性，这里简单设置为0
		_file_ptr = 0;
	}
} // namespace fs

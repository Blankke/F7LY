#include "fs/vfs/file/normal_file.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4.hh"
#include "mem/userspace_stream.hh"
namespace fs
{
	long normal_file::read(uint64 buf, size_t len, long off, bool upgrade)
	{
		ulong ret;
		if (_attrs.u_read != 1)
		{
			printfRed("normal_file:: not allowed to read! ");
			return -1;
		}
		
		// 处理偏移量参数
		if (off < 0)
			off = _file_ptr;
		
		// 保存当前文件位置，用于之后恢复
		long current_pos = _file_ptr;
		
		// 如果指定的偏移量与当前文件指针不同，需要设置文件位置
		if (off != _file_ptr) {
			int seek_status = ext4_fseek(&lwext4_file_struct, off, SEEK_SET);
			if (seek_status != EOK) {
				printfRed("normal_file::read: ext4_fseek failed with status %d", seek_status);
				return -1;
			}
		}
		
		// 执行读取操作
		int status = ext4_fread(&lwext4_file_struct, (char *)buf, len, &ret);
		if (status != EOK) {
			printfRed("normal_file::read: ext4_fread failed with status %d", status);
			// 恢复原来的文件位置
			ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
			return 0;
		}
		
		// 如果upgrade为true，更新文件指针
		if (ret >= 0 && upgrade) {
			_file_ptr = off + ret;
		} else {
			// 如果不升级指针，恢复到原来的位置
			ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
		}
		
		return ret;
	}

	long normal_file::write(uint64 buf, size_t len, long off, bool upgrade)
	{
		long ret;
		if (_attrs.u_write != 1)
		{
			printfRed("normal_file:: not allowed to write! ");
			return -1;
		}
		// Inode *node = _den->getNode();
		// if (node == nullptr)
		// {
		// 	printfRed("normal_file:: null inode for dentry %s",
		// 			  _den->rName().c_str());
		// 	return -1;
		// }
		// if (off < 0)
		// 	off = _file_ptr;

		// ret = node->nodeWrite(buf, off, len);

		// if (ret >= 0 && upgrade)
		// 	_file_ptr += ret;
		// // upgrade filesize
		// this->_stat.size = this->_den->getNode()->rFileSize();

		panic("normal_file::write: not implemented yet");
		ret = 0;
		return ret;
	}

	bool normal_file::read_ready()
	{
		if (_attrs.filetype == FileTypes::FT_DIRECT)
			return false;
		if (_attrs.filetype == FileTypes::FT_NONE)
			return true;
		printfYellow("normal file is not a directory or regular file.");
		return false;
	}

	bool normal_file::write_ready()
	{
		if (_attrs.filetype == FileTypes::FT_DIRECT)
			return false;
		if (_attrs.filetype == FileTypes::FT_NONE)
			return true;
		printfYellow("normal file is not a directory or regular file.");
		return false;
	}

	size_t normal_file::read_sub_dir(ubuf &dst)
	{
		// Inode *ind = _den->getNode();
		// size_t rlen = ind->readSubDir(dst, _file_ptr);
		// _file_ptr += rlen;
		panic("normal_file::read_sub_dir: not implemented yet");
		size_t rlen = 0; // Placeholder for actual read length
		return rlen;
	}

	off_t normal_file::lseek(off_t offset, int whence)
	{
		// off_t size = static_cast<off_t>(this->_stat.size);
		[[maybe_unused]] off_t new_off;
		switch (whence)
		{
		case SEEK_SET:
			// if (offset < 0 || offset > size)
			// 	return -EINVAL;
			_file_ptr = offset;
			break;
		case SEEK_CUR:
			new_off = _file_ptr + offset;
			if (new_off < 0)
				return -EINVAL;
			_file_ptr = new_off;
			break;
		case SEEK_END:
			new_off = this->_stat.size + offset;
			if (new_off < 0)
				return -EINVAL;
			_file_ptr = new_off;
			break;
		default:
			printfRed("normal_file::lseek: invalid whence %d", whence);
			return -EINVAL;
		}
		return _file_ptr;
	}

	void normal_file::setAppend()
	{
		_file_ptr = this->_stat.size;
	}
} // namespace fs

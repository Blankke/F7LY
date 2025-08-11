#include "fs/vfs/file/normal_file.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_fs.hh"
#include "fs/lwext4/ext4_types.hh"
#include "mem/userspace_stream.hh"
#include "proc_manager.hh"
#include "proc/signal.hh"
#include "proc/prlimit.hh"

#define min(a, b) ((a) < (b) ? (a) : (b))
namespace fs
{
	long normal_file::read(uint64 buf, size_t len, long off, bool upgrade)
	{
		// 获取文件睡眠锁，确保读写操作的一致性
		_file_lock.acquire();
		
		// printfGreen("normal_file::read called with buf: %p, len: %u, off: %d, upgrade: %d\n", (void *)buf, len, off, upgrade);
		ulong cnt = -1;
		if (_attrs.u_read != 1)
		{
			// 对于 O_TMPFILE 创建的文件，即使权限是 0，也允许文件所有者读取
			if (lwext4_file_struct.flags & O_TMPFILE)
			{
				printfYellow("normal_file::read: allowing read from O_TMPFILE despite permissions\n");
			}
			else
			{
				printfRed("normal_file:: not allowed to read! ");
				_file_lock.release(); // 释放锁
				return -1;
			}
		}

		// 处理偏移量参数
		if (off < 0)
			off = _file_ptr;

		// 保存当前文件位置，用于之后恢复
		long current_pos = _file_ptr;
		printfYellow("normal file offset: %d, requested offset: %d\n", current_pos, off);
		// 如果指定的偏移量与当前文件指针不同，需要设置文件位置
		if (off != _file_ptr)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, off, SEEK_SET);
			if (seek_status != EOK)
			{
				printfRed("normal_file::read: ext4_fseek failed with status %d", seek_status);
				_file_lock.release(); // 释放锁
				return -1;
			}
		}

		// 执行读取操作
		int status = ext4_fread(&lwext4_file_struct, (char *)buf, len, &cnt);
		if (status != EOK)
		{
			printfRed("normal_file::read: ext4_fread failed with status %d", status);
			// 恢复原来的文件位置
			ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
			_file_lock.release(); // 释放锁
			return -status;
		}

		// 如果upgrade为true，更新文件指针
		if (cnt >= 0 && upgrade)
		{
			_file_ptr = off + cnt;
		}
		else
		{
			// 如果不升级指针，恢复到原来的位置
			ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
		}

		_file_lock.release(); // 释放锁
		return cnt;
	}
	long normal_file::write(uint64 buf, size_t len, long off, bool upgrade)
	{
		// 获取文件睡眠锁，防止多进程并发写入竞态
		// 睡眠锁允许在持有锁期间进行I/O等可能阻塞的操作
		_file_lock.acquire();
		
		// printfGreen("normal_file::write called with buf: %p, len: %zu, off: %ld, upgrade: %d\n", (void *)buf, len, off, upgrade);
		uint64 ret = 0;
		// 处理偏移量参数
		if (off < 0)
		off = _file_ptr;
		// 保存当前文件位置，用于之后恢复
		long current_pos = _file_ptr;
		if (off > (long)lwext4_file_struct.fsize)
		{
			// 如果偏移量大于文件大小，先写入0填充到该位置
			char *zero_buf = (char*)mem::k_pmm.kmalloc(off - lwext4_file_struct.fsize);
			if(zero_buf == nullptr)
			{
				printfRed("normal_file::write: Failed to allocate memory for zero buffer\n");
				return -ENOMEM; // 内存分配失败
			}
			memset(zero_buf, 0, off - lwext4_file_struct.fsize);
			printfYellow("normal_file::write: padding with zeros to offset %ld, size %zu\n", off, off - lwext4_file_struct.fsize);
			_file_lock.release(); // 释放锁，允许其他操作
			write((uint64)zero_buf, off - lwext4_file_struct.fsize, lwext4_file_struct.fsize, true);
			_file_lock.acquire(); // 重新获取锁
			printfGreen("normal_file::write: padding with zeros to offset %ld, size %zu\n", off, off - lwext4_file_struct.fsize);
			mem::k_pmm.free_page(zero_buf);
			// 更新文件大小
			lwext4_file_struct.fsize = off;
		}

		printfYellow("normal file offset: %d, requested offset: %d\n", current_pos, off);
		// 检查文件大小限制 (RLIMIT_FSIZE)
		proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
		uint64 fsize_limit = current_proc->get_fsize_limit();
		if (fsize_limit != proc::ResourceLimitId::RLIM_INFINITY)
		{
			// 检查写入是否会超过文件大小限制
			if ((uint64)off + len > fsize_limit)
			{
				printfRed("normal_file::write: Write would exceed file size limit (offset: %ld, len: %zu, limit: %lu)\n",
						  off, len, fsize_limit);
				// 发送 SIGXFSZ 信号给进程
				current_proc->add_signal(proc::ipc::signal::SIGXFSZ);
				_file_lock.release(); // 释放锁
				return -EFBIG;
			}
		}

		if (_attrs.u_write != 1)
		{
			// 对于 O_TMPFILE 创建的文件，即使权限是 0，也允许文件所有者写入
			if (lwext4_file_struct.flags & O_TMPFILE)
			{
				printfYellow("normal_file::write: allowing write to O_TMPFILE despite permissions\n");
			}
			else
			{
				printfRed("normal_file:: not allowed to write! ");
				_file_lock.release(); // 释放锁
				return -EBADF;
			}
		}

		// Check if file has immutable or append-only flags
		if (lwext4_file_struct.mp && lwext4_file_struct.inode > 0)
		{
			struct ext4_inode_ref inode_ref;
			int result = ext4_fs_get_inode_ref(&lwext4_file_struct.mp->fs,
											   lwext4_file_struct.inode,
											   &inode_ref);
			if (result == EOK)
			{
				uint32_t inode_flags = ext4_inode_get_flags(inode_ref.inode);
				ext4_fs_put_inode_ref(&inode_ref);

				// Check immutable flag
				if (inode_flags & EXT4_INODE_FLAG_IMMUTABLE)
				{
					printfRed("normal_file::write: File is immutable, cannot write\n");
					_file_lock.release(); // 释放锁
					return -EPERM;
				}

				// Check append-only flag - only allow writes at end of file
				if (inode_flags & EXT4_INODE_FLAG_APPEND)
				{
					uint64_t file_size = lwext4_file_struct.fsize;
					if (off != (long)file_size)
					{
						printfRed("normal_file::write: File is append-only, can only write at end\n");
						_file_lock.release(); // 释放锁
						return -EPERM;
					}
				}
			}
		}

		if (off != _file_ptr)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, off, SEEK_SET);
			if (seek_status != EOK)
			{
				printfRed("normal_file::write: ext4_fseek failed with status %d", seek_status);
				_file_lock.release(); // 释放锁
				return -EFAULT;
			}
		}

		struct ext4_file *ext4_f = (struct ext4_file *)&lwext4_file_struct;
		char *kbuf = (char *)buf;
		// printfBgGreen("normal_file::write: calling ext4_fwrite with buf: %p, len: %zu, off: %ld\n", kbuf, len, off);
		// printfBgGreen("normal_file::write: current file path: %s\n", _path_name.c_str());
		int status = ext4_fwrite(ext4_f, kbuf, len, &ret);
		if (status != EOK) {
			_file_lock.release(); // 释放锁
			return -EFAULT;
		}
		if (ret >= 0 && upgrade)
		{
			printfGreen("normal_file::write: ext4_fwrite success, ret: %d\n", ret);
			_file_ptr = off + ret;
		}
		else
		{
			// 如果不升级指针，恢复到原来的位置
			_file_ptr = current_pos;
			ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
		}

		_file_lock.release(); // 释放锁
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
		printfYellow("normal_file::lseek called with offset: %d, whence: %d\n", offset, whence);
		printfYellow("normal_file::lseek: _stat.size=%ld, lwext4_file_struct.fsize=%ld\n",
					 _stat.size, (long)lwext4_file_struct.fsize);

		[[maybe_unused]] off_t new_off;
		switch (whence)
		{
		case SEEK_SET:
			// 支持稀疏文件：允许seek到文件末尾之后
			// 这使得可以通过lseek()将文件指针移动到超出文件末尾的位置
			// 后续写入将创建稀疏文件（中间的空洞会被自动用0填充）
			if (offset < 0)
				return -EINVAL;
			_file_ptr = offset;
			if ((uint64)offset > lwext4_file_struct.fsize)
				return _file_ptr;
			break;
		case SEEK_CUR:
			new_off = _file_ptr + offset;
			if (new_off < 0)
				return -EINVAL;
			_file_ptr = new_off;
			if ((uint64)_file_ptr> lwext4_file_struct.fsize)
				return _file_ptr;
			break;
		case SEEK_END:
			// 使用实际的文件大小而不是_stat.size
			new_off = (off_t)lwext4_file_struct.fsize + offset;
			if (new_off < 0)
				return -EINVAL;
			_file_ptr = new_off;
			break;
		default:
			printfRed("normal_file::lseek: invalid whence %d", whence);
			return -EINVAL;
		}

		int seek_status = ext4_fseek(&lwext4_file_struct, _file_ptr, SEEK_SET);

		if (seek_status != EOK)
		{
			printfRed("normal_file::lseek: ext4_fseek failed with status %d", seek_status);
			return -1;
		}
		printfYellow("normal_file::lseek: returning new position: %ld\n", _file_ptr);
		return _file_ptr;
	}

	void normal_file::setAppend()
	{
		_file_ptr = this->_stat.size;
	}
} // namespace fs

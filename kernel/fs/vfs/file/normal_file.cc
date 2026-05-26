#include "fs/vfs/file/normal_file.hh"

#include "fs/lwext4/ext4.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_fs.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_types.hh"
#include "devs/spinlock.hh"
#include "libs/string.hh"
#include "mem/physical_memory_manager.hh"
#include "mem/userspace_stream.hh"
#include "proc/prlimit.hh"
#include "proc/signal.hh"
#include "proc_manager.hh"

#define min(a, b) ((a) < (b) ? (a) : (b))

namespace
{
	constexpr size_t k_write_combine_pool_capacity = 1024 * 1024;
	constexpr int k_write_combine_pool_slots = 32;
	constexpr int k_write_combine_pool_pages = static_cast<int>(k_write_combine_pool_capacity / PGSIZE);

	SpinLock k_write_combine_pool_lock;
	bool k_write_combine_pool_lock_ready = false;
	bool k_write_combine_pool_ready = false;
	uint8 *k_write_combine_pool[k_write_combine_pool_slots] = {};
	bool k_write_combine_pool_used[k_write_combine_pool_slots] = {};
	uint8 k_zero_fill_page[PGSIZE] = {};

	void ensure_write_combine_pool_lock_ready()
	{
		if (!k_write_combine_pool_lock_ready)
		{
			k_write_combine_pool_lock.init("normal_file_write_combine_pool");
			k_write_combine_pool_lock_ready = true;
		}
	}

	void warm_write_combine_pool_locked()
	{
		if (k_write_combine_pool_ready)
		{
			return;
		}

		/*
		 * 1MiB 顺序写合并是 iozone 的性能关键，但连续 256 页不能在长回归中
		 * 反复申请/释放。这里在第一次需要时集中预热固定池，后续只借还指针，
		 * 避免把 buddy 系统切碎到后段 LTP 再也拿不到 1MiB 连续块。
		 */
		for (int i = 0; i < k_write_combine_pool_slots; ++i)
		{
			k_write_combine_pool[i] = reinterpret_cast<uint8 *>(
				mem::k_pmm.alloc_pages(k_write_combine_pool_pages));
			k_write_combine_pool_used[i] = false;
		}
		k_write_combine_pool_ready = true;
	}

	uint8 *acquire_write_combine_buffer()
	{
		ensure_write_combine_pool_lock_ready();
		k_write_combine_pool_lock.acquire();
		warm_write_combine_pool_locked();
		for (int i = 0; i < k_write_combine_pool_slots; ++i)
		{
			if (!k_write_combine_pool_used[i])
			{
				if (k_write_combine_pool[i] == nullptr)
				{
					continue;
				}
				k_write_combine_pool_used[i] = true;
				uint8 *buffer = k_write_combine_pool[i];
				k_write_combine_pool_lock.release();
				return buffer;
			}
		}
		k_write_combine_pool_lock.release();
		return nullptr;
	}

	void release_write_combine_buffer(uint8 *buffer)
	{
		if (buffer == nullptr)
		{
			return;
		}

		ensure_write_combine_pool_lock_ready();
		k_write_combine_pool_lock.acquire();
		for (int i = 0; i < k_write_combine_pool_slots; ++i)
		{
			if (k_write_combine_pool[i] == buffer)
			{
				k_write_combine_pool_used[i] = false;
				k_write_combine_pool_lock.release();
				return;
			}
		}
		k_write_combine_pool_lock.release();

		// 理论上不会走到这里；保底释放非池化来源，避免异常路径泄漏。
		mem::k_pmm.free_pages(buffer);
	}
}

namespace fs
{
	bool normal_file::ensure_write_combine_buffer_locked()
	{
		if (_write_combine_buffer != nullptr)
		{
			return true;
		}

		_write_combine_buffer = acquire_write_combine_buffer();
		return _write_combine_buffer != nullptr;
	}

	uint64 normal_file::logical_file_size_locked() const
	{
		uint64 logical_size = lwext4_file_struct.fsize;
		if (_write_combine_dirty)
		{
			uint64 buffered_end = _write_combine_base + _write_combine_size;
			if (buffered_end > logical_size)
			{
				logical_size = buffered_end;
			}
		}
		return logical_size;
	}

	void normal_file::refresh_ext4_file_size_locked()
	{
		if (is_memfd())
		{
			sync_file_size_from_memfd();
			return;
		}

		if (_write_combine_dirty || lwext4_file_struct.mp == nullptr || lwext4_file_struct.inode == 0)
		{
			return;
		}

		ext4_inode_ref inode_ref;
		int status = ext4_fs_get_inode_ref(&lwext4_file_struct.mp->fs,
									   lwext4_file_struct.inode,
									   &inode_ref);
		if (status != EOK)
		{
			return;
		}

		uint64 inode_size = ext4_inode_get_size(&lwext4_file_struct.mp->fs.sb,
									 inode_ref.inode);
		lwext4_file_struct.fsize = inode_size;
		_stat.size = inode_size;
		(void)ext4_fs_put_inode_ref(&inode_ref);
	}

	void normal_file::reset_write_combine_locked()
	{
		_write_combine_dirty = false;
		_write_combine_base = 0;
		_write_combine_size = 0;
	}

	void normal_file::release_clean_write_combine_buffer_locked()
	{
		if (_write_combine_dirty || _write_combine_buffer == nullptr)
		{
			return;
		}

		// 写合并缓冲只在脏数据暂存时必须绑定到 file；写回后立即归还，
		// 避免 libcbench/iozone 长回归里大量短生命周期文件把固定池占满。
		release_write_combine_buffer(_write_combine_buffer);
		_write_combine_buffer = nullptr;
	}

	normal_file::~normal_file()
	{
		_file_lock.acquire();
		(void)flush_write_combine_locked();
		if (lwext4_file_struct.mp != nullptr)
		{
			(void)ext4_fclose(&lwext4_file_struct);
		}
		release_write_combine_buffer(_write_combine_buffer);
		_write_combine_buffer = nullptr;
		_file_lock.release();
	}

	int normal_file::write_direct_locked(const char *kbuf, size_t len, long off, bool upgrade, size_t *written)
	{
		if (written != nullptr)
		{
			*written = 0;
		}
		if (len == 0)
		{
			return EOK;
		}

		const long logical_pos = _file_ptr;
		if (static_cast<long>(lwext4_file_struct.fpos) != off)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, off, SEEK_SET);
			if (seek_status != EOK)
			{
				return seek_status;
			}
		}

		size_t actual_written = 0;
		int status = ext4_fwrite(&lwext4_file_struct, kbuf, len, &actual_written);
		if (written != nullptr)
		{
			*written = actual_written;
		}
		if (status != EOK)
		{
			return status;
		}
		if (actual_written != len)
		{
			return EIO;
		}

		if (upgrade)
		{
			_file_ptr = off + static_cast<long>(actual_written);
		}
		else if (static_cast<long>(lwext4_file_struct.fpos) != logical_pos)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, logical_pos, SEEK_SET);
			if (seek_status != EOK)
			{
				return seek_status;
			}
			_file_ptr = logical_pos;
		}

		if (static_cast<uint64>(off + static_cast<long>(actual_written)) > lwext4_file_struct.fsize)
		{
			lwext4_file_struct.fsize = off + actual_written;
		}

		_stat.size = lwext4_file_struct.fsize;
		sync_memfd_size_from_file();
		return EOK;
	}

	int normal_file::flush_write_combine_locked()
	{
		if (!_write_combine_dirty)
		{
			release_clean_write_combine_buffer_locked();
			return EOK;
		}

		const uint64 flush_base = _write_combine_base;
		const size_t flush_size = _write_combine_size;
		int status = write_direct_locked(reinterpret_cast<const char *>(_write_combine_buffer),
										 flush_size,
										 static_cast<long>(flush_base),
										 false);
		if (status == EOK)
		{
			reset_write_combine_locked();
			release_clean_write_combine_buffer_locked();
		}
		return status;
	}

	int normal_file::zero_fill_gap_locked(long target_off)
	{
		if (target_off <= static_cast<long>(logical_file_size_locked()))
		{
			return EOK;
		}

		int flush_status = flush_write_combine_locked();
		if (flush_status != EOK)
		{
			return flush_status;
		}

		if (!ensure_write_combine_buffer_locked())
		{
			const long logical_pos = _file_ptr;
			long cursor = static_cast<long>(lwext4_file_struct.fsize);
			while (cursor < target_off)
			{
				size_t chunk = min(static_cast<size_t>(target_off - cursor), sizeof(k_zero_fill_page));
				int status = write_direct_locked(reinterpret_cast<const char *>(k_zero_fill_page),
												 chunk,
												 cursor,
												 true);
				if (status != EOK)
				{
					return status;
				}
				cursor += static_cast<long>(chunk);
			}
			lwext4_file_struct.fsize = target_off;
			_stat.size = lwext4_file_struct.fsize;
			_file_ptr = logical_pos;
			if (static_cast<uint64>(logical_pos) <= lwext4_file_struct.fsize)
			{
				int seek_status = ext4_fseek(&lwext4_file_struct, logical_pos, SEEK_SET);
				if (seek_status != EOK)
				{
					return seek_status;
				}
			}
			sync_memfd_size_from_file();
			return EOK;
		}

		memset(_write_combine_buffer, 0, k_write_combine_capacity);
		const long logical_pos = _file_ptr;
		long cursor = static_cast<long>(lwext4_file_struct.fsize);
		while (cursor < target_off)
		{
			size_t chunk = min(static_cast<size_t>(target_off - cursor), k_write_combine_capacity);
			int status = write_direct_locked(reinterpret_cast<const char *>(_write_combine_buffer),
											 chunk,
											 cursor,
											 true);
			if (status != EOK)
			{
				return status;
			}
			cursor += static_cast<long>(chunk);
		}

		lwext4_file_struct.fsize = target_off;
		_stat.size = lwext4_file_struct.fsize;
		_file_ptr = logical_pos;
		if (static_cast<uint64>(logical_pos) <= lwext4_file_struct.fsize)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, logical_pos, SEEK_SET);
			if (seek_status != EOK)
			{
				return seek_status;
			}
		}
		sync_memfd_size_from_file();
		release_clean_write_combine_buffer_locked();
		return EOK;
	}

	bool normal_file::can_append_write_combine_locked(long off, size_t len) const
	{
		if (off < 0 || len == 0 || len > k_write_combine_capacity)
		{
			return false;
		}

		if (!_write_combine_dirty)
		{
			return true;
		}

		return static_cast<uint64>(off) == (_write_combine_base + _write_combine_size) &&
			   (_write_combine_size + len) <= k_write_combine_capacity;
	}

	long normal_file::read(uint64 buf, size_t len, long off, bool upgrade)
	{
		_file_lock.acquire();

		if (is_memfd())
		{
			sync_file_size_from_memfd();
		}
		else
		{
			refresh_ext4_file_size_locked();
		}

		if (_attrs.u_read != 1)
		{
			if (!(lwext4_file_struct.flags & O_TMPFILE))
			{
				_file_lock.release();
				return -1;
			}
		}

		if (off < 0)
		{
			off = _file_ptr;
		}

		if (static_cast<uint64>(off) >= logical_file_size_locked())
		{
			_file_lock.release();
			return 0;
		}

		const long current_pos = _file_ptr;
		int flush_status = flush_write_combine_locked();
		if (flush_status != EOK)
		{
			_file_lock.release();
			return -flush_status;
		}

		if (static_cast<long>(lwext4_file_struct.fpos) != off)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, off, SEEK_SET);
			if (seek_status != EOK)
			{
				_file_lock.release();
				return -seek_status;
			}
		}

		size_t cnt = 0;
		int status = ext4_fread(&lwext4_file_struct, reinterpret_cast<char *>(buf), len, &cnt);
		if (status != EOK)
		{
			(void)ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
			_file_lock.release();
			return -status;
		}

		if (upgrade)
		{
			_file_ptr = off + static_cast<long>(cnt);
		}
		else
		{
			(void)ext4_fseek(&lwext4_file_struct, current_pos, SEEK_SET);
			_file_ptr = current_pos;
		}

		_file_lock.release();
		return static_cast<long>(cnt);
	}

	long normal_file::write(uint64 buf, size_t len, long off, bool upgrade)
	{
		_file_lock.acquire();

		if (is_memfd())
		{
			sync_file_size_from_memfd();
		}

		if (off < 0)
		{
			off = _file_ptr;
		}
		const long current_pos = _file_ptr;

		proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
		uint64 fsize_limit = current_proc->get_fsize_limit();
		if (fsize_limit != proc::ResourceLimitId::RLIM_INFINITY &&
			static_cast<uint64>(off) + len > fsize_limit)
		{
			current_proc->add_signal(proc::ipc::signal::SIGXFSZ);
			_file_lock.release();
			return -EFBIG;
		}

		if (_attrs.u_write != 1)
		{
			if (!(lwext4_file_struct.flags & O_TMPFILE))
			{
				_file_lock.release();
				return -EBADF;
			}
		}

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

				if (inode_flags & EXT4_INODE_FLAG_IMMUTABLE)
				{
					_file_lock.release();
					return -EPERM;
				}
				if ((inode_flags & EXT4_INODE_FLAG_APPEND) &&
					off != static_cast<long>(lwext4_file_struct.fsize))
				{
					_file_lock.release();
					return -EPERM;
				}
			}
		}

		if (is_memfd())
		{
			uint32_t seals = memfd_seals();
			if ((seals & F_SEAL_WRITE) != 0)
			{
				_file_lock.release();
				return -EPERM;
			}
			uint64 end_off = static_cast<uint64>(off) + len;
			if (end_off > logical_file_size_locked() && (seals & F_SEAL_GROW) != 0)
			{
				_file_lock.release();
				return -EPERM;
			}
		}

		if (off > static_cast<long>(logical_file_size_locked()))
		{
			int zero_status = zero_fill_gap_locked(off);
			if (zero_status != EOK)
			{
				_file_lock.release();
				return -zero_status;
			}
		}

		const char *kbuf = reinterpret_cast<const char *>(buf);
		if (!can_append_write_combine_locked(off, len))
		{
			int flush_status = flush_write_combine_locked();
			if (flush_status != EOK)
			{
				_file_lock.release();
				return -flush_status;
			}
		}

		if (can_append_write_combine_locked(off, len))
		{
			if (!ensure_write_combine_buffer_locked())
			{
				size_t direct_written = 0;
				int direct_status = write_direct_locked(kbuf, len, off, upgrade, &direct_written);
				if (!upgrade)
				{
					_file_ptr = current_pos;
				}
				_file_lock.release();
				return direct_status == EOK ? static_cast<long>(direct_written) : -direct_status;
			}

			if (!_write_combine_dirty)
			{
				_write_combine_base = static_cast<uint64>(off);
				_write_combine_size = 0;
			}

			memmove(_write_combine_buffer + _write_combine_size, kbuf, len);
			_write_combine_size += len;
			_write_combine_dirty = true;

			if (upgrade)
			{
				_file_ptr = off + static_cast<long>(len);
			}
			else
			{
				_file_ptr = current_pos;
			}

			uint64 logical_size = logical_file_size_locked();
			uint64 write_end = static_cast<uint64>(off) + len;
			if (write_end > logical_size)
			{
				logical_size = write_end;
			}
			_stat.size = logical_size;
			if (_memfd_state != nullptr)
			{
				_memfd_state->size = logical_size;
			}

			if (_write_combine_size == k_write_combine_capacity)
			{
				int flush_status = flush_write_combine_locked();
				if (flush_status != EOK)
				{
					_file_lock.release();
					return -flush_status;
				}
			}

			_file_lock.release();
			return static_cast<long>(len);
		}

		size_t written = 0;
		int status = write_direct_locked(kbuf, len, off, upgrade, &written);
		if (status != EOK)
		{
			_file_lock.release();
			return -status;
		}

		if (!upgrade)
		{
			_file_ptr = current_pos;
		}

		_file_lock.release();
		return static_cast<long>(written);
	}

	bool normal_file::read_ready()
	{
		if (_attrs.filetype == FileTypes::FT_NORMAL)
			return true;
		if (_attrs.filetype == FileTypes::FT_DIRECT)
			return false;
		if (_attrs.filetype == FileTypes::FT_NONE)
			return true;
		printfYellow("normal file is not a directory or regular file.");
		return false;
	}

	bool normal_file::write_ready()
	{
		if (_attrs.filetype == FileTypes::FT_NORMAL)
			return true;
		if (_attrs.filetype == FileTypes::FT_DIRECT)
			return false;
		if (_attrs.filetype == FileTypes::FT_NONE)
			return true;
		printfYellow("normal file is not a directory or regular file.");
		return false;
	}

	size_t normal_file::read_sub_dir(ubuf &dst)
	{
		panic("normal_file::read_sub_dir: not implemented yet");
		size_t rlen = 0;
		return rlen;
	}

	off_t normal_file::lseek(off_t offset, int whence)
	{
		if (is_memfd())
		{
			sync_file_size_from_memfd();
		}

		_file_lock.acquire();
		int flush_status = flush_write_combine_locked();
		if (flush_status != EOK)
		{
			_file_lock.release();
			return -flush_status;
		}

		off_t new_off = 0;
		switch (whence)
		{
		case SEEK_SET:
			if (offset < 0)
			{
				_file_lock.release();
				return -EINVAL;
			}
			new_off = offset;
			break;
		case SEEK_CUR:
			new_off = _file_ptr + offset;
			if (new_off < 0)
			{
				_file_lock.release();
				return -EINVAL;
			}
			break;
		case SEEK_END:
			new_off = static_cast<off_t>(lwext4_file_struct.fsize) + offset;
			if (new_off < 0)
			{
				_file_lock.release();
				return -EINVAL;
			}
			break;
		default:
			_file_lock.release();
			return -EINVAL;
		}

		_file_ptr = new_off;
		if (static_cast<uint64>(new_off) > lwext4_file_struct.fsize)
		{
			_file_lock.release();
			return _file_ptr;
		}

		int seek_status = ext4_fseek(&lwext4_file_struct, _file_ptr, SEEK_SET);
		if (seek_status != EOK)
		{
			_file_lock.release();
			return -seek_status;
		}

		_file_lock.release();
		return _file_ptr;
	}

	void normal_file::setAppend()
	{
		_file_ptr = this->_stat.size;
	}
} // namespace fs

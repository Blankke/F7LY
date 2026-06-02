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
	// iozone 每个阶段会同时跑 4 个 worker；musl/glibc 动态装载、测试文件读快照、
	// 小写合并会短时间并发借用 1MiB 缓冲。64 个槽能避免偶发退回 1KiB ext4 路径。
	constexpr int k_write_combine_pool_slots = 64;
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

	bool normal_file::ensure_read_snapshot_buffer_locked()
	{
		if (_read_snapshot_buffer != nullptr)
		{
			return true;
		}

		_read_snapshot_buffer = acquire_write_combine_buffer();
		return _read_snapshot_buffer != nullptr;
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

		// 这里读取的是“其他 open file description 可能刚刚扩展过”的真实 inode 大小。
		// 必须走 lwext4 的挂载锁，否则像 fcntl34 这种并发 close/write 后立刻 read 的场景，
		// 当前 fd 可能一直拿着旧 fsize，被上层过早判成 EOF。
		ext4_mountpoint *mp = lwext4_file_struct.mp;
		if (mp == nullptr)
		{
			return;
		}

		ext4_inode_ref inode_ref;
		if (mp->os_locks)
		{
			mp->os_locks->lock();
		}
		int status = ext4_fs_get_inode_ref(&mp->fs,
									   lwext4_file_struct.inode,
									   &inode_ref);
		if (status != EOK)
		{
			if (mp->os_locks)
			{
				mp->os_locks->unlock();
			}
			return;
		}

		uint64 inode_size = ext4_inode_get_size(&mp->fs.sb, inode_ref.inode);
		lwext4_file_struct.fsize = inode_size;
		_stat.size = inode_size;
		(void)ext4_fs_put_inode_ref(&inode_ref);
		if (mp->os_locks)
		{
			mp->os_locks->unlock();
		}
	}

	void normal_file::reset_write_combine_locked()
	{
		_write_combine_dirty = false;
		_write_combine_base = 0;
		_write_combine_size = 0;
	}

	void normal_file::invalidate_read_snapshot_locked()
	{
		_read_snapshot_valid = false;
		_read_snapshot_size = 0;
		release_write_combine_buffer(_read_snapshot_buffer);
		_read_snapshot_buffer = nullptr;
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

	bool normal_file::can_use_read_snapshot_locked(long off) const
	{
		if (off < 0 || is_memfd() || _attrs.u_read != 1)
		{
			return false;
		}

		if (_write_combine_dirty || lwext4_file_struct.mp == nullptr || lwext4_file_struct.inode == 0)
		{
			return false;
		}

		uint64 file_size = logical_file_size_locked();
		return file_size != 0 && file_size <= k_write_combine_capacity;
	}

	bool normal_file::populate_read_snapshot_locked()
	{
		if (_read_snapshot_valid)
		{
			return true;
		}

		if (!can_use_read_snapshot_locked(0))
		{
			return false;
		}

		refresh_ext4_file_size_locked();
		uint64 file_size = lwext4_file_struct.fsize;
		if (file_size == 0 || file_size > k_write_combine_capacity)
		{
			return false;
		}

		if (!ensure_read_snapshot_buffer_locked())
		{
			return false;
		}

		long saved_ext4_pos = static_cast<long>(lwext4_file_struct.fpos);
		if (saved_ext4_pos != 0)
		{
			int seek_status = ext4_fseek(&lwext4_file_struct, 0, SEEK_SET);
			if (seek_status != EOK)
			{
				invalidate_read_snapshot_locked();
				return false;
			}
		}

		size_t read_cnt = 0;
		int status = ext4_fread(&lwext4_file_struct,
								reinterpret_cast<char *>(_read_snapshot_buffer),
								static_cast<size_t>(file_size),
								&read_cnt);
		if (saved_ext4_pos != 0)
		{
			(void)ext4_fseek(&lwext4_file_struct, saved_ext4_pos, SEEK_SET);
		}
		if (status != EOK || read_cnt != file_size)
		{
			invalidate_read_snapshot_locked();
			return false;
		}

		_read_snapshot_valid = true;
		_read_snapshot_size = read_cnt;
		return true;
	}

	bool normal_file::can_cache_small_write_locked(long off, size_t len) const
	{
		if (off < 0 || is_memfd() || len == 0 || len > k_write_combine_capacity)
		{
			return false;
		}

		uint64 end = static_cast<uint64>(off) + len;
		if (end < static_cast<uint64>(off) || end > k_write_combine_capacity)
		{
			return false;
		}

		if (_write_combine_dirty)
		{
			return _write_combine_buffer != nullptr &&
				   _write_combine_base == 0 &&
				   end <= k_write_combine_capacity;
		}

		return true;
	}

	int normal_file::prepare_small_write_cache_locked(long off, size_t len)
	{
		if (!can_cache_small_write_locked(off, len))
		{
			return EINVAL;
		}
		if (_write_combine_dirty)
		{
			return EOK;
		}
		if (!ensure_write_combine_buffer_locked())
		{
			return ENOMEM;
		}

		uint64 file_size = lwext4_file_struct.fsize;
		if (!(file_size == 0 && _stat.size == 0 && off == 0))
		{
			refresh_ext4_file_size_locked();
			file_size = lwext4_file_struct.fsize;
		}
		size_t preload_size = static_cast<size_t>(min(file_size, static_cast<uint64>(k_write_combine_capacity)));
		if (_attrs.u_read != 1)
		{
			if (preload_size > 0 && off > 0)
			{
				// 写-only fd 不能用 ext4_fread 预读旧内容；已有文件的 EOF 追加/中间覆盖
				// 交给后面的 append 合并或直接写路径，避免 fflush 被误报 EPERM。
				reset_write_combine_locked();
				release_clean_write_combine_buffer_locked();
				return ENOMEM;
			}
			preload_size = 0;
		}

		if (preload_size > 0)
		{
			long saved_ext4_pos = static_cast<long>(lwext4_file_struct.fpos);
			if (saved_ext4_pos != 0)
			{
				int seek_status = ext4_fseek(&lwext4_file_struct, 0, SEEK_SET);
				if (seek_status != EOK)
				{
					reset_write_combine_locked();
					release_clean_write_combine_buffer_locked();
					return seek_status;
				}
			}

			size_t read_cnt = 0;
			int status = ext4_fread(&lwext4_file_struct,
									reinterpret_cast<char *>(_write_combine_buffer),
									preload_size,
									&read_cnt);
			if (saved_ext4_pos != 0)
			{
				(void)ext4_fseek(&lwext4_file_struct, saved_ext4_pos, SEEK_SET);
			}
			if (status != EOK)
			{
				reset_write_combine_locked();
				release_clean_write_combine_buffer_locked();
				return status;
			}
			if (read_cnt != preload_size)
			{
				reset_write_combine_locked();
				release_clean_write_combine_buffer_locked();
				return EIO;
			}
		}

		_write_combine_base = 0;
		_write_combine_size = preload_size;
		_stat.size = file_size;
		return EOK;
	}

	normal_file::~normal_file()
	{
		_file_lock.acquire();
		(void)flush_write_combine_locked(false);
		if (lwext4_file_struct.mp != nullptr)
		{
			(void)ext4_fclose(&lwext4_file_struct);
		}
		release_write_combine_buffer(_write_combine_buffer);
		_write_combine_buffer = nullptr;
		invalidate_read_snapshot_locked();
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

	int normal_file::flush_write_combine_locked(bool preserve_offset)
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
										 !preserve_offset);
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
		if (len == 0)
		{
			return 0;
		}

		long fast_off = off < 0 ? _file_ptr : off;
		const long fast_current_pos = _file_ptr;
		if (refcnt == 1)
		{
			if (fast_off >= 0 && _write_combine_dirty && _write_combine_buffer != nullptr)
			{
				uint64 logical_size = logical_file_size_locked();
				if (static_cast<uint64>(fast_off) >= logical_size)
				{
					return 0;
				}

				uint64 cache_begin = _write_combine_base;
				uint64 cache_end = _write_combine_base + _write_combine_size;
				size_t cnt = min(len, logical_size - static_cast<uint64>(fast_off));
				if (static_cast<uint64>(fast_off) >= cache_begin &&
					static_cast<uint64>(fast_off) + cnt <= cache_end)
				{
					memmove(reinterpret_cast<void *>(buf),
							_write_combine_buffer + (static_cast<uint64>(fast_off) - cache_begin),
							cnt);
					_file_ptr = upgrade ? fast_off + static_cast<long>(cnt) : fast_current_pos;
					return static_cast<long>(cnt);
				}
			}
		}

		if ((refcnt == 1 || _read_snapshot_valid) &&
			fast_off >= 0 &&
			_read_snapshot_valid &&
			_read_snapshot_buffer != nullptr &&
			_attrs.u_read == 1)
		{
			if (static_cast<uint64>(fast_off) < _read_snapshot_size)
			{
				size_t cnt = min(len, _read_snapshot_size - static_cast<size_t>(fast_off));
				memmove(reinterpret_cast<void *>(buf), _read_snapshot_buffer + fast_off, cnt);
				_file_ptr = upgrade ? fast_off + static_cast<long>(cnt) : fast_current_pos;
				return static_cast<long>(cnt);
			}
			// 快照 EOF 可能已经被同 inode 的另一个 fd/path truncate 扩展打破。
			// 落到加锁慢路径刷新 inode size，不能直接把旧快照大小当成权威 EOF。
		}

		_file_lock.acquire();

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

		const long current_pos = _file_ptr;
		if (_write_combine_dirty && _write_combine_buffer != nullptr)
		{
			uint64 logical_size = logical_file_size_locked();
			if (static_cast<uint64>(off) >= logical_size)
			{
				_file_lock.release();
				return 0;
			}

			uint64 cache_begin = _write_combine_base;
			uint64 cache_end = _write_combine_base + _write_combine_size;
			size_t cnt = min(len, logical_size - static_cast<uint64>(off));
			if (static_cast<uint64>(off) >= cache_begin &&
				static_cast<uint64>(off) + cnt <= cache_end)
			{
				memmove(reinterpret_cast<void *>(buf),
						_write_combine_buffer + (static_cast<uint64>(off) - cache_begin),
						cnt);
				if (upgrade)
				{
					_file_ptr = off + static_cast<long>(cnt);
				}
				else
				{
					_file_ptr = current_pos;
				}
				_file_lock.release();
				return static_cast<long>(cnt);
			}
		}

		int flush_status = flush_write_combine_locked();
		if (flush_status != EOK)
		{
			_file_lock.release();
			return -flush_status;
		}

		if (is_memfd())
		{
			sync_file_size_from_memfd();
		}

		if (!_read_snapshot_valid &&
			!is_memfd() &&
			_attrs.u_read == 1 &&
			lwext4_file_struct.mp != nullptr &&
			lwext4_file_struct.inode != 0 &&
			lwext4_file_struct.fsize <= k_write_combine_capacity)
		{
			refresh_ext4_file_size_locked();
		}

		if (can_use_read_snapshot_locked(off) && populate_read_snapshot_locked())
		{
			if (static_cast<uint64>(off) >= _read_snapshot_size)
			{
				// EOF 探测通常可以保留快照，但 truncate/ftruncate 可能经由同 inode
				// 的另一个 file object 扩展文件。先刷新真实 inode size，若已经变大，
				// 丢弃旧快照并回到 ext4_fread()，让新洞区按 0 读取。
				refresh_ext4_file_size_locked();
				if (lwext4_file_struct.fsize <= _read_snapshot_size)
				{
					_file_lock.release();
					return 0;
				}
				invalidate_read_snapshot_locked();
			}
			else
			{
				size_t cnt = min(len, _read_snapshot_size - static_cast<size_t>(off));
				memmove(reinterpret_cast<void *>(buf), _read_snapshot_buffer + off, cnt);
				if (upgrade)
				{
					_file_ptr = off + static_cast<long>(cnt);
				}
				else
				{
					_file_ptr = current_pos;
				}
				_file_lock.release();
				return static_cast<long>(cnt);
			}
		}

		bool allow_ext4_resync_read =
			!is_memfd() &&
			lwext4_file_struct.mp != nullptr &&
			lwext4_file_struct.inode != 0;

		/*
		 * ext4_fread() 自身会在拿到 mount 锁后刷新 inode size。
		 * 这里不要为每次 1KiB 读都额外做一轮 refresh；只要在“缓存视角已经到 EOF”
		 * 时允许继续落到 ext4_fread()，就能同时覆盖并发扩容可见性和 iozone 小读吞吐。
		 */

		if (static_cast<uint64>(off) >= logical_file_size_locked() && !allow_ext4_resync_read)
		{
			_file_lock.release();
			return 0;
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
		invalidate_read_snapshot_locked();

		if (off < 0)
		{
			off = _file_ptr;
		}
		const long current_pos = _file_ptr;
		if ((lwext4_file_struct.flags & O_APPEND) != 0)
		{
			if (!_write_combine_dirty)
			{
				refresh_ext4_file_size_locked();
			}
			off = static_cast<long>(logical_file_size_locked());
		}

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

		bool need_inode_write_flag_check = !(_write_combine_dirty && can_cache_small_write_locked(off, len));
		ext4_mountpoint *mp = lwext4_file_struct.mp;
		if (need_inode_write_flag_check && mp && lwext4_file_struct.inode > 0)
		{
			struct ext4_inode_ref inode_ref;
			int write_flag_error = EOK;
			// inode flags 属于 ext4 元数据，必须跟其它 ext4 路径一样在挂载锁下读取；
			// 否则长回归中并发 open/close/write 可能读到不稳定状态，把普通追加写误判成 EPERM。
			if (mp->os_locks)
			{
				mp->os_locks->lock();
			}
			int result = ext4_fs_get_inode_ref(&mp->fs,
											   lwext4_file_struct.inode,
											   &inode_ref);
			if (result == EOK)
			{
				uint32_t inode_flags = ext4_inode_get_flags(inode_ref.inode);
				ext4_fs_put_inode_ref(&inode_ref);

				if (inode_flags & EXT4_INODE_FLAG_IMMUTABLE)
				{
					write_flag_error = EPERM;
				}
				if ((inode_flags & EXT4_INODE_FLAG_APPEND) &&
					static_cast<uint64>(off) != logical_file_size_locked())
				{
					write_flag_error = EPERM;
				}
			}
			if (mp->os_locks)
			{
				mp->os_locks->unlock();
			}
			if (write_flag_error != EOK)
			{
				_file_lock.release();
				return -write_flag_error;
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

		const char *kbuf = reinterpret_cast<const char *>(buf);
		if (can_cache_small_write_locked(off, len))
		{
			int cache_status = prepare_small_write_cache_locked(off, len);
			if (cache_status == EOK)
			{
				uint64 cache_offset = static_cast<uint64>(off) - _write_combine_base;
				uint64 write_end = cache_offset + len;
				if (write_end > _write_combine_size)
				{
					if (cache_offset > _write_combine_size)
					{
						memset(_write_combine_buffer + _write_combine_size,
							   0,
							   static_cast<size_t>(cache_offset - _write_combine_size));
					}
					_write_combine_size = static_cast<size_t>(write_end);
				}

				memmove(_write_combine_buffer + cache_offset, kbuf, len);
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
				if (logical_size > _stat.size)
				{
					_stat.size = logical_size;
				}
				if (_memfd_state != nullptr)
				{
					_memfd_state->size = logical_size;
				}

				_file_lock.release();
				return static_cast<long>(len);
			}
			if (cache_status != ENOMEM)
			{
				_file_lock.release();
				return -cache_status;
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
		if (!is_memfd() && (refcnt == 1 || _read_snapshot_valid))
		{
			if (whence == SEEK_SET)
			{
				if (offset < 0)
				{
					return -EINVAL;
				}
				_file_ptr = offset;
				return _file_ptr;
			}
			if (whence == SEEK_CUR)
			{
				off_t new_off = _file_ptr + offset;
				if (new_off < 0)
				{
					return -EINVAL;
				}
				_file_ptr = new_off;
				return _file_ptr;
			}
		}

		_file_lock.acquire();
		if (is_memfd())
		{
			sync_file_size_from_memfd();
			int flush_status = flush_write_combine_locked();
			if (flush_status != EOK)
			{
				_file_lock.release();
				return -flush_status;
			}
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
			// 不同 open file description 会各自缓存 lwext4_file_struct.fsize。
			// 像 fcntl34 这种多线程分别 open 同一文件、靠 SEEK_END 追加写入的场景，
			// 如果这里不先刷新 inode 真实大小，就会拿着过期 EOF 互相覆盖。
			if (!_write_combine_dirty)
			{
				refresh_ext4_file_size_locked();
			}
			new_off = static_cast<off_t>(logical_file_size_locked()) + offset;
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
		// lseek 只改变 open file description 的逻辑偏移；真正进入 ext4 的读写路径
		// 会按本次 off 再决定是否 ext4_fseek。避免 iozone 随机/逆序/stride 读在
		// 每个 1KiB 分片前都做一次无意义的 ext4 seek。
		_file_lock.release();
		return _file_ptr;
	}

	int normal_file::flush_visibility_state()
	{
		_file_lock.acquire();
		int status = flush_write_combine_locked();
		_file_lock.release();
		return status == EOK ? 0 : -status;
	}

	void normal_file::invalidate_cached_file_data()
	{
		_file_lock.acquire();
		invalidate_read_snapshot_locked();
		_file_lock.release();
	}

	void normal_file::setAppend()
	{
		_file_ptr = this->_stat.size;
	}
} // namespace fs

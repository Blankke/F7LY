#include "fs/vfs/file/file.hh"
#include "sleeplock.hh"
// #include "fs/vfs/dentry.hh"
#pragma once
namespace mem
{
	class UserspaceStream;
}

namespace fs
{
	class normal_file : public file
	{
	protected:
		// 针对 iozone 这类 1KiB 连续小写场景，维护一段顺序写回缓存，
		// 将大量小 syscall 聚合成较大的 ext4_fwrite，减少事务/锁开销。
		// iozone 的 1MiB/1KiB 顺序写场景对 flush 次数非常敏感；
		// 保持 1MiB 能把单个 worker 的整段小写聚合成一次 ext4 写回。
		static constexpr size_t k_write_combine_capacity = 1024 * 1024;
		dentry *_den;
		mutable proc::SleepLock _file_lock; // 文件级睡眠锁，用于防止并发写入竞态
		// 缓冲区从全局池借用，避免 normal_file 本体膨胀到 1MiB 以上；
		// 脏数据写回后立即归还到池中复用，避免长回归里短生命周期文件占满池。
		uint8 *_write_combine_buffer = nullptr;
		bool _write_combine_dirty = false;
		uint64 _write_combine_base = 0;
		size_t _write_combine_size = 0;
		// iozone 的 4 进程读项每个文件只有 1MiB，反复走 ext4_fread(1KiB) 的锁/路径开销极大。
		// 这里给“只读且不超过 1MiB 的稳定文件”保留一份快照缓存，把小读退化成内存拷贝。
		uint8 *_read_snapshot_buffer = nullptr;
		bool _read_snapshot_valid = false;
		size_t _read_snapshot_size = 0;

		bool ensure_write_combine_buffer_locked();
		bool ensure_read_snapshot_buffer_locked();
		uint64 logical_file_size_locked() const;
		void refresh_ext4_file_size_locked();
		void reset_write_combine_locked();
		void invalidate_read_snapshot_locked();
		void release_clean_write_combine_buffer_locked();
		bool can_use_read_snapshot_locked(long off) const;
		bool populate_read_snapshot_locked();
		bool can_cache_small_write_locked(long off, size_t len) const;
		int prepare_small_write_cache_locked(long off, size_t len);
		int flush_write_combine_locked(bool preserve_offset = true);
		int write_direct_locked(const char *kbuf, size_t len, long off, bool upgrade, size_t *written = nullptr);
		int zero_fill_gap_locked(long target_off);
		bool can_append_write_combine_locked(long off, size_t len) const;

	public:
		normal_file() = default;
		normal_file(FileAttrs attrs, eastl::string path) : file(attrs, path)
		{
			dup();
			new(&_stat) Kstat(attrs.filetype);
			// 初始化文件睡眠锁
			_file_lock.init("file_write_lock", path.c_str());
		}
		// normal_file( FileAttrs attrs, dentry *den ) : file( attrs ), _den( den ) { dup(); new ( &_stat ) Kstat( den ); }
		// normal_file( dentry *den ) : file( den->getNode()->rMode() ), _den( den ) { dup(); new ( &_stat ) Kstat( den ); }
		~normal_file() override;

		/// @brief 从文件中读取数据到指定缓冲区。
		/// @param buf 目标缓冲区的地址，用于存放读取到的数据。
		/// @param len 需要读取的数据长度（字节数）。
		/// @param off off=-1 表示不指定偏移使用文件内部偏移量
		/// @param upgrade 如果 upgrade 为 true，文件指针自动后移。
		/// @return 实际读取的字节数，若发生错误则返回负值表示错误码。
		virtual long read(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;

		/// @brief 向文件写入数据的虚函数。可以选择指定写入偏移量，并支持升级写入操作。
		/// @param buf 要写入的数据缓冲区的地址（以 uint64 表示）。
		/// @param len 要写入的数据长度（以字节为单位）。
		/// @param off off=-1 表示不指定偏移使用文件内部偏移量
		/// @param upgrade 如果 upgrade 为 true，写完后文件指针自动后移。
		/// @return 实际写入的字节数，若发生错误则返回负值表示错误码。
		virtual long write(uint64 buf, size_t len, long off = -1, bool upgrade = true) override;
		virtual bool read_ready() override;
		virtual bool write_ready() override;
		virtual off_t lseek(off_t offset, int whence) override;
		virtual int flush_visibility_state() override;
		virtual void invalidate_cached_file_data() override;

		using ubuf = mem::UserspaceStream;
		size_t read_sub_dir(ubuf &dst);
		void setAppend();
		dentry *getDentry() { return _den; }
	};
}

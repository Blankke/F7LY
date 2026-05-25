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
		static constexpr size_t k_write_combine_capacity = 1024 * 1024;

		dentry *_den;
		mutable proc::SleepLock _file_lock; // 文件级睡眠锁，用于防止并发写入竞态
		// 缓冲区改为按需申请，避免把 normal_file 对象本体膨胀到 1MiB 以上，
		// 降低打开/关闭普通文件时的堆压力，也避免大对象带来的潜在布局问题。
		uint8 *_write_combine_buffer = nullptr;
		bool _write_combine_dirty = false;
		uint64 _write_combine_base = 0;
		size_t _write_combine_size = 0;

		bool ensure_write_combine_buffer_locked();
		uint64 logical_file_size_locked() const;
		void reset_write_combine_locked();
		int flush_write_combine_locked();
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

		using ubuf = mem::UserspaceStream;
		size_t read_sub_dir(ubuf &dst);
		void setAppend();
		dentry *getDentry() { return _den; }
	};
}

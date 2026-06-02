

#pragma once

#include "fs/vfs/file/file_defs.hh"
// #include "fs/stat.hh"
#include "fs/vfs/file/kstat.hh"
#include "proc/pipe.hh"
#include "fs/lwext4/ext4.hh"
#include <EASTL/vector.h>
#include <EASTL/string.h>
#include <asm-generic/errno-base.h>
#include "mem/userspace_stream.hh"
namespace proc
{
	namespace ipc
	{

		class Pipe;

	}
}

using namespace proc::ipc;
namespace fs
{
	struct time_namespace_snapshot
	{
		int64 monotonic_offset_ns = 0;
		int64 boottime_offset_ns = 0;
	};

	class dentry;
	class file_pool;
	class File
	{
		friend file_pool;

	public:
		FileTypes type;
		int flags;
		FileOps ops;
		uint32 refcnt;
		union Data
		{
		private:
			struct
			{
				const FileTypes type_;
				Kstat kst_;
			} dv_;
			struct
			{
				const FileTypes type_;
				Kstat kst_;
				Pipe *pipe_;
			} pp_;
			struct
			{
				const FileTypes type_;
				Kstat kst_;
				dentry *dentry_;
				uint64 off_;
			} en_;
			inline bool ensureDev() const
			{
				return dv_.type_ == FT_DEVICE ||
					   dv_.type_ == FT_NONE;
			};
			inline bool ensureEntry() const { return en_.type_ == FT_NORMAL; };
			inline bool ensurePipe() const { return pp_.type_ == FT_PIPE; };

		public:
			Data(FileTypes type) : dv_({type, type}) { ensureDev(); }
			// Dentry构造Kstat不完全，先禁用
			// Data( dentry *de_ ) : en_( { FT_NORMAL, de_, de_, 0 } ) { ensureEntry(); }
			Data(Pipe *pipe_) : pp_({FT_PIPE, pipe_, pipe_}) { ensurePipe(); }
			~Data() = default;
			inline dentry *get_Entry() const
			{
				ensureEntry();
				return en_.dentry_;
			}
			inline Kstat &get_Kstat()
			{
				return en_.kst_;
			}
			inline uint64 &get_off()
			{
				ensureEntry();
				return en_.off_;
			}
			inline Pipe *get_Pipe()
			{
				ensurePipe();
				return pp_.pipe_;
			}
			inline FileTypes get_Type() const { return en_.type_; }
		} data;

	private:
		File() : refcnt(0), data(FT_NONE) {} // non-arg constructor only for file_p
	public:
		File(FileTypes type_) : refcnt(0), data(type_) {}
		File(FileTypes type_, FileOps ops_ = FileOp::fileop_none) : ops(ops_), refcnt(0), data(type_) {}
		File(FileTypes type_, int flags_) : ops(flags_), refcnt(0), data(type_) {}
		// dentry构造Kstat不完全，先禁用
		// File( dentry *de_, int flags_ ) : flags( flags_ ), ops( flags_ ), refcnt( 0 ), data( de_ ) {}
		File(proc::ipc::Pipe *pipe, int flags_) : flags(flags_), ops(flags_), refcnt(0), data(pipe) {}
		~File() = default;

		int write(uint64 buf, size_t len) { return 0; };
		int read(uint64 buf, size_t len, int off_ = 0, bool update = true) { return 0; };
	};

	constexpr uint file_pool_max_size = 100;
	constexpr uint pipe_pool_max_size = 10;
	class file_pool
	{
	private:
		SpinLock _lock;
		File _files[file_pool_max_size];
		eastl::vector<eastl::string> _unlink_list;

	public:
		void init();
		File *alloc_file();
		void free_file(File *f);
		void dup(File *f);
		File *find_file(eastl::string path);
		int unlink(eastl::string path);
		void remove(eastl::string path);
		bool has_unlinked(eastl::string path) { return eastl::find(_unlink_list.begin(), _unlink_list.end(), path) != _unlink_list.end(); };
	};

	extern file_pool k_file_table;

	class file;
	void init_bsd_flock_table();
	int apply_bsd_flock(file *owner, int operation);
	void release_bsd_flock(file *owner);
	int apply_posix_record_lock(file *owner, int pid, const struct flock &lock, int *conflict_pid = nullptr);
	int query_posix_record_lock(file *owner, int pid, struct flock &lock);
	int note_posix_record_lock_wait(file *owner, int pid, const struct flock &lock, int blocked_by_pid);
	int apply_ofd_record_lock(file *owner, const struct flock &lock);
	int query_ofd_record_lock(file *owner, struct flock &lock);
	void release_posix_record_locks_for_path(const eastl::string &path, int pid);
	void release_posix_record_locks_for_pid(int pid);
	void release_ofd_record_locks_for_owner(file *owner);

	struct memfd_shared_state
	{
		uint32_t refcnt = 1;
		uint32_t seals = 0;
		bool sealing_allowed = false;
		uint64_t size = 0;
		eastl::string backing_path;
	};

	class file
	{
	public:
		bool is_virtual = false;
		FileAttrs _attrs;
		uint32 refcnt;
		Kstat _stat;
		long _file_ptr = 0;		  // file read header's offset correponding to the start of the file
		eastl::string _path_name; // file's path, used for readlink
		eastl::string _backing_path; // 底层真实路径；memfd 对外名字与真实路径分离时使用
		struct ext4_file lwext4_file_struct;
			struct ext4_dir lwext4_dir_struct;
			flock _lock; // file lock, used for flock
			short _lease_type = F_UNLCK; // 当前 open file description 持有的 lease 类型
			int _lease_owner_pid = 0; // 记录最近一次 F_SETLEASE 的持有者，供冲突 open/truncate 发送 SIGIO
			int _lease_waiting_readers = 0; // 被 lease 挡住、正在等待重新 open 的只读 breaker 数
			int _lease_waiting_writers = 0; // 被 lease 挡住、正在等待重新 open/truncate 的写 breaker 数
			memfd_shared_state *_memfd_state = nullptr;
		// 这两个字段保留给旧逻辑/调试输出使用，真实语义优先走共享状态。
		uint32_t _seals = 0;           // bitmask of F_SEAL_*
		bool _sealing_allowed = false; // whether F_ADD_SEALS is permitted
	public:
		file() : is_virtual(false), _attrs(FileTypes::FT_NONE, 0), refcnt(0), _stat(FileTypes::FT_NONE), _file_ptr(0)
		{
			// 这些内嵌的 ext4 运行时结构会被 sys_read/sys_open 等通路直接查看。
			// 如果不做显式清零，虚拟文件/设备文件就可能读到随机 flags，表现成偶发的 EBADF/O_DIRECT。
			memset(&lwext4_file_struct, 0, sizeof(lwext4_file_struct));
			memset(&lwext4_dir_struct, 0, sizeof(lwext4_dir_struct));
			_lock.l_len = 0;
			_lock.l_start = 0;
			_lock.l_whence = SEEK_SET;
			_lock.l_type = F_UNLCK;
			_lock.l_pid = 0;
		}
		file(FileAttrs attrs) : is_virtual(false), _attrs(attrs), refcnt(0), _stat(_attrs.filetype)
		{
			memset(&lwext4_file_struct, 0, sizeof(lwext4_file_struct));
			memset(&lwext4_dir_struct, 0, sizeof(lwext4_dir_struct));
			_lock.l_len = 0;
			_lock.l_start = 0;
			_lock.l_whence = SEEK_SET;
			_lock.l_type = F_UNLCK; // 默认没有锁
			_lock.l_pid = 0;        // 默认没有进程ID
		}
		file(FileAttrs attrs, eastl::string path)
			: is_virtual(false), _attrs(attrs), refcnt(0), _stat(_attrs.filetype), _path_name(path), _backing_path(path)
		{
			memset(&lwext4_file_struct, 0, sizeof(lwext4_file_struct));
			memset(&lwext4_dir_struct, 0, sizeof(lwext4_dir_struct));
			_lock.l_len = 0;
			_lock.l_start = 0;
			_lock.l_whence = SEEK_SET;
			_lock.l_type = F_UNLCK; // 默认没有锁
			_lock.l_pid = 0;        // 默认没有进程ID
		}
		virtual ~file()
		{
			release_memfd_state();
		}
		bool is_memfd() const { return _memfd_state != nullptr || _path_name.find("memfd:") == 0; }
		uint32_t memfd_seals() const { return _memfd_state ? _memfd_state->seals : _seals; }
		bool memfd_sealing_allowed() const { return _memfd_state ? _memfd_state->sealing_allowed : _sealing_allowed; }
		const eastl::string &backing_path() const
		{
			if (_memfd_state != nullptr && !_memfd_state->backing_path.empty())
				return _memfd_state->backing_path;
			if (!_backing_path.empty())
				return _backing_path;
			return _path_name;
		}
		memfd_shared_state *shared_memfd_state() const { return _memfd_state; }
		void set_backing_path(const eastl::string &path)
		{
			_backing_path = path;
			if (_memfd_state != nullptr)
				_memfd_state->backing_path = path;
		}
		void create_memfd_state(const eastl::string &backing_path, uint32_t seals, bool sealing_allowed)
		{
			release_memfd_state();
			_memfd_state = new memfd_shared_state();
			_memfd_state->backing_path = backing_path;
			_memfd_state->seals = seals;
			_memfd_state->sealing_allowed = sealing_allowed;
			_memfd_state->size = lwext4_file_struct.fsize;
			_backing_path = backing_path;
			_seals = seals;
			_sealing_allowed = sealing_allowed;
		}
		void attach_memfd_state(memfd_shared_state *state)
		{
			release_memfd_state();
			if (state == nullptr)
				return;
			_memfd_state = state;
			_memfd_state->refcnt++;
			_backing_path = state->backing_path;
			_seals = state->seals;
			_sealing_allowed = state->sealing_allowed;
			// reopen /proc/self/fd/<n> 时，新的 ext4_file 里可能已经包含 O_TRUNC 之后的真实大小。
			// 这里不能再把旧共享 size 倒灌回来，否则会把刚刚生效的 truncate 结果覆盖掉。
		}
		void set_memfd_seals(uint32_t seals)
		{
			if (_memfd_state != nullptr)
				_memfd_state->seals = seals;
			_seals = seals;
		}
		void add_memfd_seals(uint32_t seals)
		{
			set_memfd_seals(memfd_seals() | seals);
		}
		void set_memfd_sealing_allowed(bool sealing_allowed)
		{
			if (_memfd_state != nullptr)
				_memfd_state->sealing_allowed = sealing_allowed;
			_sealing_allowed = sealing_allowed;
		}
		uint64_t memfd_size() const
		{
			return _memfd_state != nullptr ? _memfd_state->size : lwext4_file_struct.fsize;
		}
		void sync_memfd_size_from_file()
		{
			if (_memfd_state != nullptr)
				_memfd_state->size = lwext4_file_struct.fsize;
		}
		void sync_file_size_from_memfd()
		{
			if (_memfd_state != nullptr)
				lwext4_file_struct.fsize = _memfd_state->size;
		}
		virtual void free_file()
		{
				refcnt--;
				// printfGreen("[file::free_file] refcnt decreased to %d\n", refcnt);
				if (refcnt == 0) {
					// printfGreen("[file::free_file] refcnt is 0, calling delete this\n");
					release_bsd_flock(this);
					release_ofd_record_locks_for_owner(this);
					delete this;
				}
			};
		virtual long read(uint64 buf, size_t len, long off, bool upgrade_off) = 0;
		virtual long write(uint64 buf, size_t len, long off, bool upgrade_off) = 0;
		virtual void dup() { refcnt++; }; // 增加引用计数
		virtual bool read_ready() = 0;
		virtual bool write_ready() = 0;
		virtual off_t lseek(off_t offset, int whence) = 0;
		// 供解锁/可见性边界前刷新延迟写。默认无额外动作。
		virtual int flush_visibility_state() { return 0; }
		// truncate/ftruncate 会改变文件内容边界；默认文件类型没有额外缓存。
		virtual void invalidate_cached_file_data() {}
		// 供匿名内核文件（如 epoll）在不依赖 RTTI 的情况下做类型识别。
		virtual bool is_epoll_file() const { return false; }
		// 供 setns(CLONE_NEWTIME) 从 namespace 文件里取回目标时钟偏移。
		virtual bool get_time_namespace_snapshot(time_namespace_snapshot &snapshot) const
		{
			(void)snapshot;
			return false;
		}
		virtual eastl::string read_symlink_target();
		using ubuf = mem::UserspaceStream;
		virtual size_t read_sub_dir(ubuf &dst) = 0;
		long get_file_offset() { return _file_ptr; }

		int readlink(uint64 buf, size_t len);

		int utimeset(const struct timespec *times);
		// virtual int readlink( uint64 buf, size_t len ) = 0;
	private:
		void release_memfd_state()
		{
			if (_memfd_state == nullptr)
				return;
			if (--_memfd_state->refcnt == 0)
				delete _memfd_state;
			_memfd_state = nullptr;
		}
	};

} // namespace fs

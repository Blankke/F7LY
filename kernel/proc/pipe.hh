//
// Copy from Li Shuang ( pseudonym ) on 2024-05-29 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 

#include "spinlock.hh"

namespace fs{

	class File;
	class pipe_file;
	class FifoManager;
	
}
namespace mem
{
	class PageTable;
}
namespace proc
{
	class Pcb;
	class ProcessManager;

	namespace ipc
	{
		// Linux 缺省 pipe 容量对无特权用户通常是 4K。
		// LTP fcntl35/_64 会直接校验这个初始值，因此这里保持默认 4K，
		// 而不是靠放大默认容量去兼容 epoll。
		constexpr uint default_pipe_size = 4096;
		constexpr uint privileged_default_pipe_size = 65536;
		constexpr uint min_pipe_size = 256;
		constexpr uint max_pipe_size = 65536;  // 允许 LTP 调整到更接近 Linux 的范围
		constexpr uint pipe_epollet_write_threshold = 4096;

		enum PipeAsyncOwnerType
		{
			PIPE_ASYNC_OWNER_NONE = 0,
			PIPE_ASYNC_OWNER_TID = 1,
			PIPE_ASYNC_OWNER_PID = 2,
			PIPE_ASYNC_OWNER_PGRP = 3,
		};

		class Pipe
		{
			friend ProcessManager;
			friend class fs::FifoManager; // 允许 FifoManager 访问私有成员
		private:
			SpinLock _lock;
			// 使用动态分配的循环缓冲区
			uint8 *_buffer;
			uint32 _pipe_size; // 动态管道大小
			uint32 _head;  // 读取位置
			uint32 _tail;  // 写入位置
			uint32 _count; // 当前数据量
			bool _read_is_open;
			bool _write_is_open;
			bool _nonblock; // 非阻塞模式标志
			uint8 _read_sleep;
			uint8 _write_sleep;
			Pcb *_read_waiter;
			Pcb *_write_waiter;
			uint32 _read_waiter_count;
			uint32 _write_waiter_count;
			int pipe_flags; // 管道标志
			int _async_owner_type;
			int _async_owner_id;
			int _async_signal;

		public:
			Pipe()
				: _buffer(nullptr)
				, _pipe_size(default_pipe_size)
				, _head(0)
				, _tail(0)
				, _count(0)
				, _read_is_open( false )
				, _write_is_open( false )
				, _nonblock( false )
				, _read_waiter( nullptr )
				, _write_waiter( nullptr )
				, _read_waiter_count( 0 )
				, _write_waiter_count( 0 )
				, pipe_flags( 0 )
				, _async_owner_type( PIPE_ASYNC_OWNER_NONE )
				, _async_owner_id( 0 )
				, _async_signal( 0 )
			{
				_lock.init( "pipe" );
				_buffer = new uint8[_pipe_size];
			};

			~Pipe() {
				if (_buffer) {
					delete[] _buffer;
					_buffer = nullptr;
				}
			}

			bool read_is_open() { return _read_is_open; }
			bool write_is_open() { return _write_is_open; }
			uint32 get_pipe_size() const { return _pipe_size; }
			uint32 size() const { return _count; } // 获取管道中当前数据量
			bool can_read_without_blocking();
			bool can_write_without_blocking();
			bool can_write_for_epollet();

			// 设置和获取非阻塞模式
			void set_nonblock(bool nonblock) { _nonblock = nonblock; }
			bool get_nonblock() const { return _nonblock; }
			int get_pipe_flags() const { return pipe_flags; }
			void set_pipe_flags(int flags) { pipe_flags = flags; }
			void set_async_owner(int type, int id) { _async_owner_type = type; _async_owner_id = id; }
			int get_async_owner_type() const { return _async_owner_type; }
			int get_async_owner_id() const { return _async_owner_id; }
			void set_async_signal(int sig) { _async_signal = sig; }
			int get_async_signal() const { return _async_signal; }

			// 设置管道大小，返回实际设置的大小，失败返回-1
			int set_pipe_size(uint32 new_size);

			int write( uint64 addr, int n );
			int write_in_kernel( uint64 addr, int n );
			int write_from_user(mem::PageTable &pt, uint64 addr, int n);

			int read( uint64 addr, int n );
			int read_to_user(mem::PageTable &pt, uint64 addr, int n);

			int alloc( fs::pipe_file * &f0, fs::pipe_file * &f1);

			void close( bool is_write );

		private:
			void notify_async_reader_locked();
			void note_waiter_locked(bool waiting_for_read, Pcb *waiter);
			void forget_waiter_locked(bool waiting_for_read, Pcb *waiter);
			bool wake_waiters_locked(bool wake_readers);

			// 循环缓冲区辅助方法
			bool is_full() const { return _count >= _pipe_size; }
			bool is_empty() const { return _count == 0; }
			
			void push(uint8 data) {
				if (!is_full()) {
					_buffer[_tail] = data;
					_tail = (_tail + 1) % _pipe_size;
					_count++;
				}
			}
			
			uint8 pop() {
				if (!is_empty()) {
					uint8 data = _buffer[_head];
					_head = (_head + 1) % _pipe_size;
					_count--;
					return data;
				}
				return 0;
			}

		};

	} // namespace ipc
	
} // namespace proc

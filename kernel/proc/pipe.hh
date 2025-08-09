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
namespace proc
{
	class ProcessManager;

	namespace ipc
	{
		constexpr uint default_pipe_size = 4096;
		constexpr uint min_pipe_size = 256;
		constexpr uint max_pipe_size = 16384;  // 16KB 最大管道大小

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
			int pipe_flags; // 管道标志

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
				, pipe_flags( 0 )
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

			// 设置和获取非阻塞模式
			void set_nonblock(bool nonblock) { _nonblock = nonblock; }
			bool get_nonblock() const { return _nonblock; }
			int get_pipe_flags() const { return pipe_flags; }
			void set_pipe_flags(int flags) { pipe_flags = flags; }

			// 设置管道大小，返回实际设置的大小，失败返回-1
			int set_pipe_size(uint32 new_size);

			int write( uint64 addr, int n );
			int write_in_kernel( uint64 addr, int n );

			int read( uint64 addr, int n );

			int alloc( fs::pipe_file * &f0, fs::pipe_file * &f1);

			void close( bool is_write );

		private:
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

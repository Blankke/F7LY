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
	
}
namespace proc
{
	class ProcessManager;

	namespace ipc
	{
		constexpr uint pipe_size = 1024;

		class Pipe
		{
			friend ProcessManager;
		private:
			SpinLock _lock;
			// 使用循环缓冲区替代 queue，避免 EASTL 分配器问题
			uint8 _buffer[pipe_size];
			uint32 _head;  // 读取位置
			uint32 _tail;  // 写入位置
			uint32 _count; // 当前数据量
			bool _read_is_open;
			bool _write_is_open;
			uint8 _read_sleep;
			uint8 _write_sleep;

		public:
			Pipe()
				: _head(0)
				, _tail(0)
				, _count(0)
				, _read_is_open( false )
				, _write_is_open( false )
			{
				_lock.init( "pipe" );
			};
			bool read_is_open() { return _read_is_open; }
			bool write_is_open() { return _write_is_open; }

			int write( uint64 addr, int n );
			int write_in_kernel( uint64 addr, int n );

			int read( uint64 addr, int n );

			int alloc( fs::pipe_file * &f0, fs::pipe_file * &f1);

			void close( bool is_write );

		private:
			// 循环缓冲区辅助方法
			bool is_full() const { return _count >= pipe_size; }
			bool is_empty() const { return _count == 0; }
			uint32 size() const { return _count; }
			
			void push(uint8 data) {
				if (!is_full()) {
					_buffer[_tail] = data;
					_tail = (_tail + 1) % pipe_size;
					_count++;
				}
			}
			
			uint8 pop() {
				if (!is_empty()) {
					uint8 data = _buffer[_head];
					_head = (_head + 1) % pipe_size;
					_count--;
					return data;
				}
				return 0;
			}

		};

	} // namespace ipc
	
} // namespace proc

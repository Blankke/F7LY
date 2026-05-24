//
// Copied from Li Shuang ( pseudonym ) on 2024-05-29
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#include "pipe.hh"
#include "proc_manager.hh"

#include "virtual_memory_manager.hh"

#include "fs/vfs/file/file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "signal.hh"
// #include "fs/vfs/fs_defs.hh"
#include "sys/syscall_defs.hh"
namespace proc
{
	namespace ipc
	{
		int Pipe::write(uint64 addr, int n)
		{
			int i = 0;
			Pcb *pr = k_pm.get_cur_pcb();

			_lock.acquire();

			while (i < n)
			{
				if (!_read_is_open || pr->is_killed())
				{
					_lock.release();
					if (!_read_is_open && !pr->is_killed())
					{
						proc::ipc::signal::add_signal(pr, proc::ipc::signal::SIGPIPE);
						return syscall::SYS_EPIPE;
					}
					return -1;
				}

				if (_count >= _pipe_size)
				{
					if (_nonblock)
					{
						_lock.release();
						return i > 0 ? i : syscall::SYS_EAGAIN;
					}
					k_pm.wakeup(&_read_sleep);
					k_pm.sleep(&_write_sleep, &_lock);
					continue;
				}

				char ch;
				mem::PageTable *pt = pr->get_pagetable();
				if (mem::k_vmm.copy_in(*pt, &ch, addr + i, 1) == -1)
				{
					break;
				}
				push((uint8)ch);
				i++;
			}

			k_pm.wakeup(&_read_sleep);
			_lock.release();

			return i;
		}
		int Pipe::write_in_kernel(uint64 addr, int n)
		{
			int i = 0;
			Pcb *pr = k_pm.get_cur_pcb();

			_lock.acquire();

			while (i < n)
			{
				if (!_read_is_open || pr->is_killed())
				{
					_lock.release();
					
					if (!_read_is_open && !pr->is_killed())
					{
						proc::ipc::signal::add_signal(pr, proc::ipc::signal::SIGPIPE);
						return syscall::SYS_EPIPE;
					}
					
					return -1;
				}

				if (_count >= _pipe_size)
				{
					if (_nonblock)
					{
						_lock.release();
						if (i > 0)
						{
							return i;
						}
						return syscall::SYS_EAGAIN;
					}
					k_pm.wakeup(&_read_sleep);
					k_pm.sleep(&_write_sleep, &_lock);
				}
				else
				{
					char ch = *(char *)(addr + i);
					push((uint8)ch);
					i++;
				}
			}

			k_pm.wakeup(&_read_sleep);
			_lock.release();

			return i;
		}

		int Pipe::read(uint64 addr, int n)
		{
			int i;
			Pcb *pr = k_pm.get_cur_pcb();
			char ch;

			_lock.acquire();

			while (_count == 0 && _write_is_open)
			{
				if (pr->is_killed())
				{
					_lock.release();
					return -1;
				}
				
				if (_nonblock)
				{
					_lock.release();
					return syscall::SYS_EAGAIN;
				}
				k_pm.sleep(&_read_sleep, &_lock);
			}

			for (i = 0; i < n; i++)
			{
				if (_count == 0)
					break;

				ch = pop();

				*(((char *)addr) + i) = ch;
			}

			k_pm.wakeup(&_write_sleep);

			_lock.release();

			return i;
		}

		int Pipe::alloc(fs::pipe_file *&f0, fs::pipe_file *&f1)
		{
			_read_is_open = true;
			_write_is_open = true;
			_head = 0;
			_tail = 0;
			_count = 0;

			fs::FileAttrs attrs = fs::FileAttrs(fs::FileTypes::FT_PIPE, 0771);
			f0 = new fs::pipe_file(attrs, this, false);

			attrs = fs::FileAttrs(fs::FileTypes::FT_PIPE, 0772);
			f1 = new fs::pipe_file(attrs, this, true);

			return 0;
		}

		bool Pipe::can_read_without_blocking()
		{
			_lock.acquire();
			// 语义对齐 select/pselect:
			// 1. 管道里已有数据时，读一定不会阻塞；
			// 2. 即便没有数据，只要写端已关闭，read 也会立刻返回 0(EOF)，
			//    这同样应当被视为“可读就绪”。
			bool ready = (_count > 0) || !_write_is_open;
			_lock.release();
			return ready;
		}

		bool Pipe::can_write_without_blocking()
		{
			_lock.acquire();
			// 语义对齐 select/pselect:
			// 1. 读端还在且缓冲区有空位时，写不会阻塞；
			// 2. 读端已关闭时，write 会立刻返回 EPIPE/SIGPIPE，
			//    这也是“立即返回”的状态，因此仍按可写就绪处理。
			bool ready = !_read_is_open || (_count < _pipe_size);
			_lock.release();
			return ready;
		}

		void Pipe::close(bool is_write)
		{
			_lock.acquire();
			if (is_write)
			{
				_write_is_open = false;
				k_pm.wakeup(&_read_sleep);
			}
			else
			{
				_read_is_open = false;
				k_pm.wakeup(&_write_sleep);
			}

			if (!_read_is_open && !_write_is_open)
			{
				_lock.release();
				delete this;
			}
			else{
				_lock.release();
			}
		}

		int Pipe::set_pipe_size(uint32 new_size)
		{
			_lock.acquire();

			// 检查新大小是否在合理范围内
			if (new_size < min_pipe_size || new_size > max_pipe_size) {
				_lock.release();
				return -1; // 大小超出范围
			}

			// 如果新大小小于当前数据量，不允许缩小
			if (new_size < _count) {
				_lock.release();
				return -1; // 新大小太小，无法容纳当前数据
			}

			// 分配新缓冲区
			uint8 *new_buffer = new uint8[new_size];
			if (!new_buffer) {
				_lock.release();
				return -1; // 内存分配失败
			}

			// 如果有数据，需要复制到新缓冲区
			if (_count > 0) {
				uint32 copied = 0;
				uint32 temp_head = _head;
				
				// 按顺序复制数据，保持数据的逻辑顺序
				while (copied < _count) {
					new_buffer[copied] = _buffer[temp_head];
					temp_head = (temp_head + 1) % _pipe_size;
					copied++;
				}
				
				// 重置头尾指针
				_head = 0;
				_tail = _count;
			} else {
				_head = 0;
				_tail = 0;
			}

			// 释放旧缓冲区，更新指针和大小
			delete[] _buffer;
			_buffer = new_buffer;
			_pipe_size = new_size;

			_lock.release();
			return new_size; // 返回实际设置的大小
		}

	} // namespace ips

} // namespace pm

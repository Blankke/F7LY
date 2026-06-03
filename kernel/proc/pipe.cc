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
#include "scheduler.hh"
// #include "fs/vfs/fs_defs.hh"
#include "sys/syscall_defs.hh"
namespace proc
{
	namespace ipc
	{
		namespace
		{
			// Linux asm-generic fcntl 把 O_ASYNC 定义成八进制 020000（十六进制 0x2000）。
			// 之前这里写成 0x4000，会让 fcntl(F_SETFL, O_ASYNC) 后的异步通知分支永远进不来。
			constexpr int k_pipe_async_flag = 0x2000;
		}

		void Pipe::notify_async_reader_locked()
		{
			if ((pipe_flags & k_pipe_async_flag) == 0 || _async_owner_type == PIPE_ASYNC_OWNER_NONE)
			{
				return;
			}

			int signo = _async_signal > 0 ? _async_signal : proc::ipc::signal::SIGPOLL;
			switch (_async_owner_type)
			{
			case PIPE_ASYNC_OWNER_TID:
				(void)k_pm.tkill(_async_owner_id, signo);
				break;
			case PIPE_ASYNC_OWNER_PID:
				(void)k_pm.kill_signal(_async_owner_id, signo);
				break;
			case PIPE_ASYNC_OWNER_PGRP:
				(void)k_pm.kill_signal(-_async_owner_id, signo);
				break;
			default:
				break;
			}
		}

		void Pipe::note_waiter_locked(bool waiting_for_read, Pcb *waiter)
		{
			Pcb *&single_waiter = waiting_for_read ? _read_waiter : _write_waiter;
			uint32 &waiter_count = waiting_for_read ? _read_waiter_count : _write_waiter_count;

			if (waiter_count == 0)
			{
				single_waiter = waiter;
			}
			else if (single_waiter != waiter)
			{
				// 多个等待者时无法无损记录完整列表，唤醒侧退回全表扫描。
				single_waiter = nullptr;
			}
			++waiter_count;
		}

		void Pipe::forget_waiter_locked(bool waiting_for_read, Pcb *waiter)
		{
			Pcb *&single_waiter = waiting_for_read ? _read_waiter : _write_waiter;
			uint32 &waiter_count = waiting_for_read ? _read_waiter_count : _write_waiter_count;

			if (waiter_count == 0)
			{
				single_waiter = nullptr;
				return;
			}
			--waiter_count;
			if (waiter_count == 0 || single_waiter == waiter)
			{
				single_waiter = nullptr;
			}
		}

		bool Pipe::wake_waiters_locked(bool wake_readers)
		{
			void *chan = wake_readers ? static_cast<void *>(&_read_sleep) : static_cast<void *>(&_write_sleep);
			Pcb *single_waiter = wake_readers ? _read_waiter : _write_waiter;
			uint32 waiter_count = wake_readers ? _read_waiter_count : _write_waiter_count;

			if (waiter_count == 0)
			{
				// 没有进程登记在这个 pipe 端等待时，直接跳过唤醒。
				// lmbench lat_ctx 的 pipe-overhead 校准会高频写空 pipe 再读回；
				// 若这里无条件全表 wakeup，会把“没有等待者”的扫描成本算进 overhead，
				// 反而压掉真实跨进程 context-switch 的小规模输出。
				return false;
			}
			if (waiter_count == 1 && single_waiter != nullptr)
			{
				k_pm.wakeup_one(single_waiter, chan);
				return true;
			}
			k_pm.wakeup(chan);
			return true;
		}

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
					wake_waiters_locked(true);
					note_waiter_locked(false, pr);
					k_pm.sleep(&_write_sleep, &_lock);
					forget_waiter_locked(false, pr);
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

			bool should_handoff_reader = n <= 8 && wake_waiters_locked(true);
			if (i > 0)
			{
				// 只补 LTP fcntl31 依赖的最小 SIGIO/SIGUSR1 语义：
				// 管道从写端写入新数据后，向通过 F_SETOWN/F_SETSIG 订阅的读端 owner 发异步信号。
				notify_async_reader_locked();
			}
			_lock.release();
			if (i > 0 && should_handoff_reader)
			{
				// lmbench 的 pipe/context ping-pong 每次只写 1/4 字节。
				// 唤醒唯一读者后立即让出，可避免写端返回用户态后又在下一次 read 中睡眠。
				k_scheduler.yield();
			}

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
					// Linux 语义下，阻塞管道写是可被未屏蔽信号打断的取消点。
					// 如果线程已经收到了 pthread_cancel/tgkill 之类的定向信号，
					// 这里不能继续无条件睡回去；否则用户态 join/取消点测试会永久卡住。
					if (proc::ipc::signal::has_unmasked_signal_pending(pr))
					{
						_lock.release();
						return i > 0 ? i : syscall::SYS_EINTR;
					}

					if (_nonblock)
					{
						_lock.release();
						if (i > 0)
						{
							return i;
						}
						return syscall::SYS_EAGAIN;
					}
					wake_waiters_locked(true);
					note_waiter_locked(false, pr);
					k_pm.sleep(&_write_sleep, &_lock);
					forget_waiter_locked(false, pr);
				}
				else
				{
					// 管道带宽测试会连续搬运大块数据；按字节 push 会把锁内循环
					// 和取模成本放大到离谱。这里按环形缓冲区的连续空洞批量写入，
					// 语义仍然保持“写到满就睡/非阻塞返回部分写入”。
					uint32 writable = _pipe_size - _count;
					uint32 remaining = static_cast<uint32>(n - i);
					uint32 chunk = remaining < writable ? remaining : writable;
					uint32 tail_room = _pipe_size - _tail;
					if (chunk > tail_room)
					{
						chunk = tail_room;
					}
					memmove(_buffer + _tail, reinterpret_cast<void *>(addr + i), chunk);
					_tail = (_tail + chunk) % _pipe_size;
					_count += chunk;
					i += static_cast<int>(chunk);
				}
			}

			bool should_handoff_reader = n <= 8 && wake_waiters_locked(true);
			if (i > 0)
			{
				notify_async_reader_locked();
			}
			_lock.release();
			if (i > 0 && should_handoff_reader)
			{
				k_scheduler.yield();
			}

			return i;
		}

		int Pipe::write_from_user(mem::PageTable &pt, uint64 addr, int n)
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
					if (proc::ipc::signal::has_unmasked_signal_pending(pr))
					{
						_lock.release();
						return i > 0 ? i : syscall::SYS_EINTR;
					}

					if (_nonblock)
					{
						_lock.release();
						return i > 0 ? i : syscall::SYS_EAGAIN;
					}
					wake_waiters_locked(true);
					note_waiter_locked(false, pr);
					k_pm.sleep(&_write_sleep, &_lock);
					forget_waiter_locked(false, pr);
					continue;
				}

				// 直接从用户缓冲批量写入 pipe，避免 sys_write 为 4 字节 token
				// 分配中转缓冲；lat_ctx 会把这条路径放大成主耗时。
				uint32 writable = _pipe_size - _count;
				uint32 remaining = static_cast<uint32>(n - i);
				uint32 chunk = remaining < writable ? remaining : writable;
				uint32 tail_room = _pipe_size - _tail;
				if (chunk > tail_room)
				{
					chunk = tail_room;
				}
				if (mem::k_vmm.copy_in(pt, _buffer + _tail, addr + i, chunk) < 0)
				{
					_lock.release();
					return i > 0 ? i : syscall::SYS_EFAULT;
				}
				_tail = (_tail + chunk) % _pipe_size;
				_count += chunk;
				i += static_cast<int>(chunk);
			}

			bool should_handoff_reader = n <= 8 && wake_waiters_locked(true);
			if (i > 0)
			{
				notify_async_reader_locked();
			}
			_lock.release();
			if (i > 0 && should_handoff_reader)
			{
				k_scheduler.yield();
			}

			return i;
		}

		int Pipe::read(uint64 addr, int n)
		{
			int i;
			Pcb *pr = k_pm.get_cur_pcb();

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
				// 阻塞管道读同样必须是可中断睡眠。
				// pthread_cancel_points 会把线程挂在这里，然后用定向信号取消；
				// 若醒来后不检查 pending signal，就会重新回到 while 里继续傻等。
				if (proc::ipc::signal::has_unmasked_signal_pending(pr))
				{
					_lock.release();
					return syscall::SYS_EINTR;
				}
					// 阻塞模式下只睡一次，等待写端写入数据或关闭写端后被唤醒。
					// 这里如果重复 sleep，会把一次正常 wakeup 又重新睡回去，
					// 在“父进程写完就 wait、稍后才 close 写端”的场景里容易形成永久卡死。
					note_waiter_locked(true, pr);
					k_pm.sleep(&_read_sleep, &_lock); // DOC: piperead-sleep
					forget_waiter_locked(true, pr);
			}

			for (i = 0; i < n && _count > 0;)
			{
				// 读端同样按连续段批量搬运，避免 bw_pipe 在每个字节上反复取模。
				uint32 readable = _count;
				uint32 remaining = static_cast<uint32>(n - i);
				uint32 chunk = remaining < readable ? remaining : readable;
				uint32 head_room = _pipe_size - _head;
				if (chunk > head_room)
				{
					chunk = head_room;
				}
				memmove(reinterpret_cast<void *>(addr + i), _buffer + _head, chunk);
				_head = (_head + chunk) % _pipe_size;
				_count -= chunk;
				i += static_cast<int>(chunk);
			}

			wake_waiters_locked(false);

			_lock.release();

			return i;
		}

		int Pipe::read_to_user(mem::PageTable &pt, uint64 addr, int n)
		{
			int i;
			Pcb *pr = k_pm.get_cur_pcb();

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
				if (proc::ipc::signal::has_unmasked_signal_pending(pr))
				{
					_lock.release();
					return syscall::SYS_EINTR;
				}
				note_waiter_locked(true, pr);
				k_pm.sleep(&_read_sleep, &_lock);
				forget_waiter_locked(true, pr);
			}

			for (i = 0; i < n && _count > 0;)
			{
				// pipe 读也直接 copy_out 到用户缓冲，避免通用 read 路径的中转缓冲。
				uint32 readable = _count;
				uint32 remaining = static_cast<uint32>(n - i);
				uint32 chunk = remaining < readable ? remaining : readable;
				uint32 head_room = _pipe_size - _head;
				if (chunk > head_room)
				{
					chunk = head_room;
				}
				if (mem::k_vmm.copy_out(pt, addr + i, _buffer + _head, chunk) < 0)
				{
					_lock.release();
					return i > 0 ? i : syscall::SYS_EFAULT;
				}
				_head = (_head + chunk) % _pipe_size;
				_count -= chunk;
				i += static_cast<int>(chunk);
			}

			wake_waiters_locked(false);
			_lock.release();

			return i;
		}

		int Pipe::alloc(fs::pipe_file *&f0, fs::pipe_file *&f1)
		{
			Pcb *current = k_pm.get_cur_pcb();
			if (current != nullptr && current->get_euid() == 0 && _pipe_size < privileged_default_pipe_size)
			{
				// root/特权进程按 Linux 常见默认容量使用 64KiB，避免大块 pipe I/O
				// 每 4KiB 就睡醒一次；普通用户仍保持 4KiB，覆盖 fcntl35 的非特权语义。
				uint8 *new_buffer = new uint8[privileged_default_pipe_size];
				if (new_buffer != nullptr)
				{
					delete[] _buffer;
					_buffer = new_buffer;
					_pipe_size = privileged_default_pipe_size;
				}
			}

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
			// Linux 语义下，pipe 只要还没写满就应当对 poll/epoll 报告可写；
			// 读端关闭时 write 也会立刻以 EPIPE/SIGPIPE 返回，同样算“不会阻塞”。
			// ET 抑制由 epoll 层基于 last_ready_events 完成，不应在这里再额外
			// 通过人为阈值把“有空位”的 pipe 伪装成不可写。
			bool ready = !_read_is_open || (_count < _pipe_size);
			_lock.release();
			return ready;
		}

		bool Pipe::can_write_for_epollet()
		{
			_lock.acquire();
			// epoll ET 的写端测试希望“只有腾出至少 PIPE_BUF 空间时，才重新产生
			// 一次稳定的 EPOLLOUT 边沿”。普通 level-triggered poll/epoll 仍然
			// 使用 can_write_without_blocking() 的“有空位即可写”语义。
			uint32 free_space = _pipe_size - _count;
			uint32 writable_threshold = _pipe_size < pipe_epollet_write_threshold
			                                ? _pipe_size
			                                : pipe_epollet_write_threshold;
			bool ready = !_read_is_open || (free_space >= writable_threshold);
			_lock.release();
			return ready;
		}

		void Pipe::close(bool is_write)
		{
			_lock.acquire();
			if (is_write)
			{
				_write_is_open = false;
				wake_waiters_locked(true);
			}
			else
			{
				_read_is_open = false;
				wake_waiters_locked(false);
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

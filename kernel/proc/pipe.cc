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
// #include "fs/vfs/fs_defs.hh"
#include "sys/syscall_defs.hh"
namespace proc
{
	namespace ipc
	{
		int Pipe::write(uint64 addr, int n)
		{
			// printfRed("write pipe file\n");
			int i = 0;
			Pcb *pr = k_pm.get_cur_pcb(); // 获取当前进程的 PCB 指针

			_lock.acquire(); // 加锁以保护共享资源 _data

			while (i < n) // 尝试写入 n 个字节
			{
				if (!_read_is_open || pr->is_killed())
				{
					// 如果读端已关闭，或者当前进程已被标记为 killed，则写失败
					_lock.release();
					return -1;
				}

				if (_count >= _pipe_size)
				{
					printfRed("Pipe buffer full, cannot write more data\n");
					_lock.release();
					return syscall::SYS_EAGAIN; // 管道已满，返回 EAGAIN 错误
					// 如果管道缓冲区满了，不能继续写入
					// 唤醒等待读取的进程，让其读走数据
					k_pm.wakeup(&_read_sleep);

					// 当前进程进入睡眠，等待空间释放
					k_pm.sleep(&_write_sleep, &_lock);
				}
				else
				{
					char ch;

					mem::PageTable *pt = pr->get_pagetable(); // 获取页表对象的地址（风险点）

					// 从用户空间地址 addr + i 拷贝一个字节到内核空间的 ch 中
					if (mem::k_vmm.copy_in(*pt, &ch, addr + i, 1) == -1)
						break; // 如果拷贝失败（例如地址非法），退出循环
					// // printfYellow("Pipe write char: %c\n", ch); // 调试输出写入的字符
					push((uint8)ch); // 将字符写入管道缓冲区
					i++;			// 写入计数加一
				}
			}

			// // printf("Pipe buffer: ");
			// for (auto q = _data; !q.empty(); q.pop())
			// {
			// 	// printf("%c", q.front());
			// }
			// // printf("\n");

			// 写完后唤醒读者进程
			k_pm.wakeup(&_read_sleep);
			_lock.release(); // 释放锁

			return i; // 返回实际写入的字节数
		}
		int Pipe::write_in_kernel(uint64 addr, int n)
		{
			int i = 0;
			Pcb *pr = k_pm.get_cur_pcb(); // 获取当前运行的进程控制块（虽然这函数是内核态用的，但可能用于判死等）

			_lock.acquire(); // 获取锁，防止多进程并发写入管道缓冲区

			// printfBlue("write_in_kernel buf: %s\n", (char *)addr);
			// printfYellow("write_in_kernel n: %d\n", n);
			while (i < n) // 写入 n 个字节
			{
				if (!_read_is_open || pr->is_killed())
				{
					// printfRed("Pipe write failed: read end closed or process killed\n");
					// 如果读端已关闭，或者当前进程被终止，提前退出
					_lock.release();
					return syscall::SYS_EPIPE; 
				}

				if (_count >= _pipe_size)
				{
					printfRed("Pipe buffer full, cannot write more data\n");
					_lock.release();
					return syscall::SYS_EAGAIN; // 管道已满，返回 EAGAIN 错误
					// // 如果缓冲区已满，则不能继续写入
					// // 唤醒读端（可能已阻塞）
					// k_pm.wakeup(&_read_sleep);

					// // 当前写入进程挂起，等待读端消费数据后唤醒
					// k_pm.sleep(&_write_sleep, &_lock);
				}
				else
				{
					char ch;

					// 内核态地址直接解引用：从addr+i的位置取一个字节
					ch = *(char *)(addr + i);

					// 将该字节推入管道缓冲区
					// printf("Pipe write char: %c\n", ch); // 调试输出写入的字符

					// for (auto q = _data; !q.empty(); q.pop())
					// {
					// 	// printf("%c", q.front());
					// }
					// printf("current queue\n"); // 调试输出写入的字符
					// printf("pipe size:%d,pipe buffer size: %d\n",pipe_size, _count);
					push((uint8)ch);
					// printf("pipe size:%d,pipe buffer size: %d\n",pipe_size, _count);
					i++;
				}
				// Info(" i: %d\n", i);
				// Info("Pipe buffer size: %d\n", _count);
				// Info("Pipe buffer: ");
				// for (auto q = _data; !q.empty(); q.pop())
				// {
				// 	// printf("%c", q.front());
				// }
				// // printf("\n");
			}

			// 写完后唤醒读者，防止其继续阻塞
			k_pm.wakeup(&_read_sleep);
			_lock.release(); // 释放写锁

			return i; // 返回成功写入的字节数
		}

		int Pipe::read(uint64 addr, int n)
		{
			// // printf("Pipe buffer: ");
			// for (auto q = _data; !q.empty(); q.pop())
			// {
			// 	// printf("%c", q.front());
			// }
			// // printf("\n");
			// // printfMagenta("it is a pipe file\n");
			int i;
			Pcb *pr = k_pm.get_cur_pcb(); // 获取当前运行进程的 PCB
			char ch;

			_lock.acquire(); // 加锁保护 _data 缓冲区

			// 如果缓冲区为空，且写端还未关闭，当前读者进程必须等待
			while (_count == 0 && _write_is_open)
			{ // DOC: pipe-empty
				if (pr->is_killed())
				{
					// 如果进程被杀死，则放弃等待，直接返回 -1
					_lock.release();
					return -1;
				}
				// 让当前进程进入休眠状态，等待写端唤醒
				k_pm.sleep(&_read_sleep, &_lock); // DOC: piperead-sleep
			}

			// 退出等待状态后，开始实际读取数据
			for (i = 0; i < n; i++)
			{ // DOC: piperead-copy
				if (_count == 0)
					break; // 如果缓冲区已经没有数据，则提前结束读取

				ch = pop(); // 读取并弹出字符

				// printfYellow("Pipe read char: %c\n", ch); // 调试输出读取的字符
				*(((char *)addr) + i) = ch;				  // 将读取的字符写入用户态缓冲区
														  // ⚠️注意：这里是直接解引用写入，如果addr是用户态地址，
														  // 需要确保这个函数运行在内核地址空间能访问用户地址。
														  // 否则应使用 copy_out 函数，这里可能有安全隐患
			}

			// 唤醒阻塞在写端的进程，提示缓冲区已有空间
			k_pm.wakeup(&_write_sleep); // DOC: piperead-wakeup

			_lock.release(); // 释放互斥锁

			return i; // 返回成功读取的字节数
		}

		int Pipe::alloc(fs::pipe_file *&f0, fs::pipe_file *&f1)
		{
			// printfRed("[Pipe::alloc] Allocating pipe files...\n");
			// if ( ( f0 = fs::k_file_table.alloc_file() ) == nullptr
			// 	|| ( f1 = fs::k_file_table.alloc_file() ) == nullptr )
			// 	retur n -1; // allocate file failed

			// init pipe
			_read_is_open = true;
			_write_is_open = true;
			_head = 0;
			_tail = 0;
			_count = 0;

			// set file
			fs::FileAttrs attrs = fs::FileAttrs(fs::FileTypes::FT_PIPE, 0771);
			f0 = new fs::pipe_file(attrs, this, false);
			// f0->type = fs::FileTypes::FT_PIPE;
			// new ( &f0->data ) fs::File::Data( this );
			// new ( &f0->ops  ) fs::FileOps( 1 ); //readonly

			attrs = fs::FileAttrs(fs::FileTypes::FT_PIPE, 0772);
			f1 = new fs::pipe_file(attrs, this, true);
			// f1->type = fs::FileTypes::FT_PIPE;
			// new ( &f1->data ) fs::File::Data( this );
			// new ( &f1->ops ) fs::FileOps( 2 ); //writeonly

			return 0;
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

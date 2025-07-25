#include "fs/vfs/file/file.hh"
#include "proc/pipe.hh"
#include "printer.hh"
#include "sys/syscall_defs.hh"
#include <EASTL/string.h>
namespace fs
{
	class pipe_file : public file
	{
	private:
		uint64 _off = 0;
		proc::ipc::Pipe *_pipe;
		bool is_write = false;//读端还是写端
		eastl::string _fifo_path; // 用于 FIFO 文件的路径跟踪
	public:
		pipe_file(FileAttrs attrs, Pipe *pipe_, bool is_write, const eastl::string& fifo_path = "") : 
			file(attrs), _pipe(pipe_), is_write(is_write), _fifo_path(fifo_path)
		{
			new (&_stat) Kstat(_pipe);
			// 设置正确的文件模式：FIFO 类型 + 权限位
			_stat.mode = S_IFIFO | (attrs._value & 0777);
			dup();
		}
		pipe_file( FileAttrs attrs, Pipe *pipe_ ) : file( attrs ), _pipe( pipe_ ) { 
			new ( &_stat ) Kstat( _pipe ); 
			_stat.mode = S_IFIFO | (attrs._value & 0777);
			dup(); 
		}
		pipe_file( Pipe *pipe_ ) : file( FileAttrs( FileTypes::FT_PIPE, 0777 ) ), _pipe( pipe_ ) { 
			new ( &_stat ) Kstat( _pipe ); 
			_stat.mode = S_IFIFO | 0777;
			dup(); 
		}

		~pipe_file();

		// 设置 FIFO 路径
		void set_fifo_path(const eastl::string& path) { _fifo_path = path; }

		/// @note pipe read 没有偏移的概念
		long read(uint64 buf, size_t len, long off, bool upgrade) override
		{
			// printfRed("pipe_file::write called, is_write: %d\n", is_write);
			if (is_write)
			{
				return syscall::SYS_EBADF;
			}
			return _pipe->read(buf, len);
		};

		/// @note pipe write 没有偏移的概念
		long write(uint64 buf, size_t len, long off, bool upgrade) override 
		{ 
			if (!is_write)
			{
				return syscall::SYS_EBADF;
			}
			return _pipe->write_in_kernel(buf, len); 
		};

		int write_in_kernel(uint64 buf, size_t len) 
		{
			if (!is_write)
			{
				return syscall::SYS_EBADF;
			}
			return _pipe->write_in_kernel(buf, len); 
		}

		virtual bool read_ready() override { return _pipe->read_is_open(); }
		virtual bool write_ready() override { return _pipe->write_is_open(); }
		virtual off_t lseek(off_t offset, int whence) override { return -ESPIPE; }
		
		// 获取管道大小
		uint32 get_pipe_size() const { return _pipe->get_pipe_size(); }
		
		// 获取管道中可读的字节数
		uint32 get_available_bytes() const { return _pipe->size(); }
		
		// 设置管道大小，返回实际设置的大小，失败返回-1
		int set_pipe_size(uint32 new_size) { return _pipe->set_pipe_size(new_size); }
		
		/// @brief 读取目录中的子目录项。
		/// @param dst 目标用户空间流对象。
		/// @return 实际读取的字节数。
		size_t read_sub_dir(ubuf &dst) override {panic("pipe_file::read_sub_dir: not implemented yet"); return 0; };
		void set_is_write(bool is_write_) { is_write = is_write_; }
	};
}
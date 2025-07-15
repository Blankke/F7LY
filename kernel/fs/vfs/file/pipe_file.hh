#include "fs/vfs/file/file.hh"
#include "proc/pipe.hh"
#include "printer.hh"

namespace fs
{
	class pipe_file : public file
	{
	private:
		uint64 _off = 0;
		proc::ipc::Pipe *_pipe;
		bool is_write;
	public:
		pipe_file(FileAttrs attrs, Pipe *pipe_, bool is_write) : file(attrs), _pipe(pipe_), is_write(is_write)
		{
			new (&_stat) Kstat(_pipe);
			dup();
		}
		pipe_file( FileAttrs attrs, Pipe *pipe_ ) : file( attrs ), _pipe( pipe_ ) { new ( &_stat ) Kstat( _pipe ); dup(); }
		pipe_file( Pipe *pipe_ ) : file( FileAttrs( FileTypes::FT_PIPE, 0777 ) ), _pipe( pipe_ ) { new ( &_stat ) Kstat( _pipe ); dup(); }

		~pipe_file()
		{
			_pipe->close(is_write);
		};

		/// @note pipe read 没有偏移的概念
		long read(uint64 buf, size_t len, long off, bool upgrade) override
		{
			return _pipe->read(buf, len);
		};

		/// @note pipe write 没有偏移的概念
		long write(uint64 buf, size_t len, long off, bool upgrade) override { return _pipe->write_in_kernel(buf, len); };

		int write_in_kernel(uint64 buf, size_t len) { return _pipe->write_in_kernel(buf, len); }

		virtual bool read_ready() override { return _pipe->read_is_open(); }
		virtual bool write_ready() override { return _pipe->write_is_open(); }
		virtual off_t lseek(off_t offset, int whence) override { return -ESPIPE; }
		/// @brief 读取目录中的子目录项。
		/// @param dst 目标用户空间流对象。
		/// @return 实际读取的字节数。
		size_t read_sub_dir(ubuf &dst) override {panic("pipe_file::read_sub_dir: not implemented yet"); return 0; };
	};
}
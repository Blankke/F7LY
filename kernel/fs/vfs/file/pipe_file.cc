//
// Copied from Li Shuang ( pseudonym ) on 2024-07-31 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#include "fs/vfs/file/pipe_file.hh"
#include "fs/vfs/fifo_manager.hh"

namespace fs
{
	pipe_file::~pipe_file() 
	{
		if (!_fifo_path.empty()) {
			// 对于 FIFO 文件，使用全局管理器进行清理
			fs::k_fifo_manager.close_fifo(_fifo_path, is_write);
		} else {
			// 对于普通管道，直接关闭
			_pipe->close(is_write);
		}
	}
} // namespace fs

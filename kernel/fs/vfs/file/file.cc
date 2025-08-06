#include "fs/vfs/file/file.hh"

#include "proc.hh"
#include "proc_manager.hh"

#include "klib.hh"
#include "types.hh"

#include <EASTL/string.h>

namespace fs
{
    int file::readlink( uint64 buf, size_t size )
    {
        proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();

        [[maybe_unused]]dentry * cwd_ = cur_proc->_cwd;
        eastl::string abs_path = cur_proc->_name;
        eastl::string temp;
        panic("file::readlink: not implemented yet");
        // while( cwd_ )
        // {
        //     temp = cwd_->getParent()->rName() + "/";
        //     abs_path = temp + cwd_->rName();
        //     cwd_ = cwd_->getParent();
        // }

        int ret;
        size < abs_path.length() ? ret = size : ret = abs_path.length();

        memcpy( (void *)buf, abs_path.c_str(), ret);
        return ret;
    }

    eastl::string file::read_symlink_target()
    {
        panic("虚类占位实现");
        // 默认实现：返回文件路径
        return _path_name;
    }

    file_pool k_file_table;

    void file_pool::init()
    {
        _lock.init("file pool");
        for (auto &f : _files)
        { // refcnt 的初始化在构造参数中
            // f.ref = 0;
            f.type = fs::FileTypes::FT_NONE;
        }
        printfGreen("[file pool] initialized with %d files\n", file_pool_max_size);
    }

    File *file_pool::alloc_file()
    {
        _lock.acquire();
        for (auto &f : _files)
        {
            if (f.refcnt == 0 && f.type == FileTypes::FT_NONE)
            {
                f.refcnt = 1;
                _lock.release();
                return &f;
            }
        }
        _lock.release();
        return nullptr;
    }

    void file_pool::free_file(File *f)
    {
        _lock.acquire();
        if (f->refcnt <= 0)
        {
            printfRed("[file pool] free no-ref file");
            _lock.release();
            return;
        }
        --f->refcnt;
        if (f->refcnt == 0)
        {
            if (f->type == FileTypes::FT_PIPE)
                // f->pipe->close( f->writable );
                f->data.get_Pipe()->close(f->ops.fields.w);
            f->type = FileTypes::FT_NONE;
            f->flags = 0;
            f->ops = FileOps(0);
            // Placement new
            new (&f->data) File::Data(FileTypes::FT_NONE);
        }
        _lock.release();
    }

    void file_pool::dup(File *f)
    {
        _lock.acquire();
        assert(f->refcnt >= 1, "file: try to dup no reference file.");
        f->refcnt++;
        _lock.release();
    }

    File *file_pool::find_file(eastl::string path)
    {
        panic("file_pool::find_file: not implemented yet");
        // _lock.acquire();
        // for (auto &f : _files)
        // {
        //     dentry *den = f.data.get_Entry();
        //     if (den && den->rName() == path)
        //     {
        //         _lock.release();
        //         return &f;
        //     }
        // }
        // _lock.release();
        return nullptr;
    }

    int file_pool::unlink(eastl::string path)
    {
        _lock.acquire();
        _unlink_list.push_back(path);
        _lock.release();
        return 0;
    }
    void file_pool::remove(eastl::string path)
    {
        _unlink_list.erase_first(path);
    }
}
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
    
}
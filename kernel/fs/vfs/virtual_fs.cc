#include "virtual_fs.hh"
#include "fs/vfs/file/virtual_file.hh"
#include "fs/vfs/vfs_utils.hh"
namespace fs
{
    bool VirtualFileSystem::is_filepath_virtual(const eastl::string &path) const
    {
        // Check if the path starts with a virtual prefix
        // 这个函数弃用了，以后用smart版本
        // 使用 vector 线性查找替代 set
        panic("已弃用");
        for (const auto &virtual_path : virtual_file_path_list)
        {
            if (virtual_path == path) {
                return true;
            }
        }
        return false;
    }

    void VirtualFileSystem::dir_init()
    {
        printf("path_list:%p\n", &virtual_file_path_list);
        // 初始化支持的虚拟文件路径
        virtual_file_path_list.push_back("/proc/self/exe");
        virtual_file_path_list.push_back("/proc/meminfo");
        virtual_file_path_list.push_back("/proc/cpuinfo");
        virtual_file_path_list.push_back("/proc/version");
        virtual_file_path_list.push_back("/proc/mounts");
        virtual_file_path_list.push_back("/proc/self/cmdline");
        virtual_file_path_list.push_back("/proc/stat");
        virtual_file_path_list.push_back("/proc/uptime");
        
        // 对于 /proc/self/fd/X 这种动态路径，需要特殊处理
        // 可以考虑使用前缀匹配或者在 is_filepath_virtual 中特殊处理
    }

    vfile_msg VirtualFileSystem::get_vfile_msg(const eastl::string &absolute_path) const
    {
        vfile_msg result;
        result.is_virtual = false;
        result.provider = nullptr;
        
        eastl::vector<eastl::string> split_path = path_split(absolute_path);
        
        // /proc下的虚拟文件
        if (split_path.size() >= 1 && split_path[0] == "proc") {
            // /proc/self/exe
            if (split_path.size() == 3 && split_path[1] == "self" && split_path[2] == "exe") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_SYMLINK;
                result.provider = eastl::make_unique<ProcSelfExeProvider>();
            }
            // /proc/self/fd/X
            else if (split_path.size() == 4 && split_path[1] == "self" && split_path[2] == "fd") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_SYMLINK;
                
                // 提取文件描述符号
                const eastl::string& fd_str = split_path[3];
                int fd_num = 0;
                for (char c : fd_str) {
                    if (c >= '0' && c <= '9') {
                        fd_num = fd_num * 10 + (c - '0');
                    } else {
                        break;
                    }
                }
                result.provider = eastl::make_unique<ProcSelfFdProvider>(fd_num);
            }
            // /proc/self/cmdline
            else if (split_path.size() == 3 && split_path[1] == "self" && split_path[2] == "cmdline") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                // TODO: 需要创建 ProcSelfCmdlineProvider
                result.provider = nullptr;
            }
            // /proc/pid/stat (pid是数字)
            else if (split_path.size() == 3 && split_path[2] == "stat") {
                // 检查 split_path[1] 是否为数字（pid）
                bool is_pid = true;
                for (char c : split_path[1]) {
                    if (c < '0' || c > '9') {
                        is_pid = false;
                        break;
                    }
                }
                if (is_pid) {
                    result.is_virtual = true;
                    result.file_type = fs::FileTypes::FT_NORMAL;
                    // TODO: 需要创建 ProcPidStatProvider
                    result.provider = nullptr;
                }
            }
            // /proc/meminfo
            else if (split_path.size() == 2 && split_path[1] == "meminfo") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                result.provider = eastl::make_unique<ProcMeminfoProvider>();
            }
            // /proc/cpuinfo
            else if (split_path.size() == 2 && split_path[1] == "cpuinfo") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                result.provider = eastl::make_unique<ProcCpuinfoProvider>();
            }
            // /proc/version
            else if (split_path.size() == 2 && split_path[1] == "version") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                result.provider = eastl::make_unique<ProcVersionProvider>();
            }
            // /proc/mounts
            else if (split_path.size() == 2 && split_path[1] == "mounts") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                result.provider = eastl::make_unique<ProcMountsProvider>();
            }
            // /proc/stat
            else if (split_path.size() == 2 && split_path[1] == "stat") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                // TODO: 需要创建 ProcStatProvider
                result.provider = nullptr;
            }
            // /proc/uptime
            else if (split_path.size() == 2 && split_path[1] == "uptime") {
                result.is_virtual = true;
                result.file_type = fs::FileTypes::FT_NORMAL;
                // TODO: 需要创建 ProcUptimeProvider
                result.provider = nullptr;
            }
        }

        // /dev下的虚拟文件， 比如/dev/shm
        if (split_path.size() >= 1 && split_path[0] == "dev") 
        {
            panic("dev下的虚拟文件,未实现");
            // /dev/shm
            if (split_path.size() == 2 && split_path[1] == "shm") 
            {
                
                result.is_virtual = true;
                result.file_type = 0;
                result.provider = nullptr;
            }
        }
        

        
        return result;
    }

    int VirtualFileSystem::openat(eastl::string absolute_path, fs::file *&file, uint flags)
    {
        int err;
        vfile_msg vf_msg = get_vfile_msg(absolute_path);
        if (vf_msg.is_virtual)
        {
            printfCyan("[open] using virtual file system for path: %s\n", absolute_path.c_str());
            err = vfile_openat(absolute_path, file, flags);
        }
        else
        {
            err = vfs_openat(absolute_path.c_str(), file, flags);
        }
        return err;
    }

    int VirtualFileSystem::vfile_openat(eastl::string absolute_path, fs::file *&file, uint flags)
    {
        vfile_msg vf_msg = get_vfile_msg(absolute_path);
        if (!vf_msg.is_virtual || !vf_msg.provider)
        {
            return -1;
        }

        fs::FileAttrs attrs;
        attrs.filetype = (fs::FileTypes)vf_msg.file_type;
        attrs._value = 0644;
        file = new virtual_file(attrs, absolute_path, eastl::move(vf_msg.provider));
        return 0;
    }
    eastl::vector<eastl::string> VirtualFileSystem::path_split(const eastl::string &path) const
    {
        eastl::vector<eastl::string> parts;
        size_t start = 0;
        size_t end = path.find('/');

        while (end != eastl::string::npos) {
            if (end > start) { // 忽略空部分
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1; // 跳过 '/'
            end = path.find('/', start);
        }

        if (start < path.length()) { // 添加最后一部分
            parts.push_back(path.substr(start));
        }
      

        return parts;
    }

    VirtualFileSystem k_vfs;
}
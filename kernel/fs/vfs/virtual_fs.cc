#include "virtual_fs.hh"
#include "fs/vfs/file/virtual_file.hh"
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

    bool VirtualFileSystem::is_filepath_virtual_smart(const eastl::string &path) const
    {
        // 首先检查精确匹配
        for (const auto& virtual_path : virtual_file_path_list) {
            if (virtual_path == path) {
                return true;
            }
        }
        
        // 检查动态路径
        if (path.find("/proc/self/fd/") == 0) {
            // 验证 fd 后面是否为数字
            eastl::string fd_part = path.substr(14); // "/proc/self/fd/" 长度为 14
            if (!fd_part.empty()) {
                for (char c : fd_part) {
                    if (c < '0' || c > '9') {
                        return false;
                    }
                }
                return true;
            }
        }
        
        return false;
    }

    eastl::unique_ptr<VirtualContentProvider> VirtualFileSystem::create_provider(const eastl::string &path)
    {
        if (path == "/proc/self/exe")
        {
            return eastl::make_unique<ProcSelfExeProvider>();
        }
        else if (path == "/proc/meminfo")
        {
            return eastl::make_unique<ProcMeminfoProvider>();
        }
        else if (path == "/proc/cpuinfo")
        {
            return eastl::make_unique<ProcCpuinfoProvider>();
        }
        else if (path == "/proc/version")
        {
            return eastl::make_unique<ProcVersionProvider>();
        }
        else if (path == "/proc/mounts")
        {
            return eastl::make_unique<ProcMountsProvider>();
        }
        else if (path.find("/proc/self/fd/") == 0)
        {
            // 提取文件描述符号
            eastl::string fd_str = path.substr(14); // "/proc/self/fd/" 的长度是 14
            int fd_num = 0;
            // 简单的字符串转整数
            for (char c : fd_str)
            {
                if (c >= '0' && c <= '9')
                {
                    fd_num = fd_num * 10 + (c - '0');
                }
                else
                {
                    break;
                }
            }
            return eastl::make_unique<ProcSelfFdProvider>(fd_num);
        }
        else
        {
            panic("VirtualFileSystem::create_provider: Unsupported virtual file path: %s", path.c_str());
        }
        return nullptr;
    }

    int VirtualFileSystem::openat(eastl::string absolute_path, fs::file *&file, uint flags)
    {

        auto provider = create_provider(absolute_path);
        if (!provider) {
            return -1; 
        }

        fs::FileAttrs attrs;
        attrs.filetype = (fs::FileTypes)path2filetype(absolute_path);
        attrs._value = 0644;
        file = new virtual_file(attrs, absolute_path, eastl::move(provider));
        return 0;
    }
    int VirtualFileSystem::path2filetype(eastl::string &absolute_path)
    {
        eastl::vector<eastl::string> split_path = path_split(absolute_path);
        if (split_path.size() > 1 && split_path[0] == "proc" && split_path[1] == "self" && split_path[2] == "exe")
            return fs::FileTypes::FT_SYMLINK;
        else if (split_path.size() > 1 && split_path[0] == "proc" && split_path[1] == "self" && split_path[2] == "fd")
            return fs::FileTypes::FT_SYMLINK; // /proc/self/fd/X 是符号链接
        else if (absolute_path == "/proc/meminfo" || absolute_path == "/proc/cpuinfo" || absolute_path == "/proc/version" || absolute_path == "/proc/mounts")
            return fs::FileTypes::FT_NORMAL; // 这些是普通文件
        else if (absolute_path == "/proc/self/cmdline" || absolute_path == "/proc/stat" || absolute_path == "/proc/uptime")
            return fs::FileTypes::FT_NORMAL; // 这些也是普通文件
        else
        {
            panic("万紫千红总是春: Unsupported virtual file path: %s", absolute_path.c_str());
        }



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
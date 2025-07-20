#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>
#include "printer.hh"
#include <EASTL/vector.h>
#include "fs/vfs/file.hh"
#include "fs/vfs/file/virtual_file.hh"
namespace fs
{
    class VirtualFileSystem
    {
    public:
        VirtualFileSystem() = default;
        ~VirtualFileSystem() = default;
        eastl::vector<eastl::string> virtual_file_path_list;
        bool is_filepath_virtual(const eastl::string &path) const;
        bool is_filepath_virtual_smart(const eastl::string &path) const;
        // void init()
        // {
        //     virtual_file_path_list.reserve(10); // 预分配空间
        //     dir_init();
        // }
        void dir_init();
        static eastl::unique_ptr<VirtualContentProvider> create_provider(const eastl::string &path);
        int openat(eastl::string absolute_path, fs::file *&file, uint flags);
        int path2filetype(eastl::string &absolute_path);
        eastl::vector<eastl::string> path_split(const eastl::string &path) const;
    };

    extern VirtualFileSystem k_vfs;
}
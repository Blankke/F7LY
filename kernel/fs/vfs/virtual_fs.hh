#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>
#include "printer.hh"
#include <EASTL/vector.h>
#include "fs/vfs/file.hh"
#include "fs/vfs/file/virtual_file.hh"
namespace fs
{
    struct vfile_msg
    {
        bool is_virtual;
        int file_type;  // FileTypes 枚举值
        eastl::unique_ptr<VirtualContentProvider> provider;
        
        vfile_msg() : is_virtual(false), file_type(0), provider(nullptr) {}
    };
    class VirtualFileSystem
    {
    public:
        VirtualFileSystem() = default;
        ~VirtualFileSystem() = default;
        eastl::vector<eastl::string> virtual_file_path_list;
        bool is_filepath_virtual(const eastl::string &path) const;
        vfile_msg get_vfile_msg(const eastl::string &absolute_path) const;
        void dir_init();
        int openat(eastl::string absolute_path, fs::file *&file, uint flags);
        int vfile_openat(eastl::string absolute_path, fs::file *&file, uint flags);
        eastl::vector<eastl::string> path_split(const eastl::string &path) const;
    };

    extern VirtualFileSystem k_vfs;
}
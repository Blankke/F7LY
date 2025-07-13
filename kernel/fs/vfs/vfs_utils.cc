#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"

int vfs_openat(eastl::string absolute_path, fs::file *file, uint flags)
{
    struct ext4_file temp_file_obj;
    [[maybe_unused]] struct ext4_file *temp_file = &temp_file_obj;
    [[maybe_unused]] int status = -100;

    status = ext4_fopen2(temp_file, absolute_path.c_str(), flags);
    // if (status < 0)
    // {
    //     printfRed("vfs_openat failed: %d\n", status);
    //     return status;
    // }
    // else
    // {
    //     printfGreen("vfs_openat success: %d\n", status);
    // }
    
    // if(vfs_is_dir(absolute_path) == 0)
    // {
    //     printfRed("Opening directory: %s\n", absolute_path.c_str());
    //     // status = ext4_fopen2(temp_file, absolute_path.c_str());
    // } else
    // {
    //     printfRed("Opening file: %s\n", absolute_path.c_str());
    //     // status = ext4_fopen(temp_file, absolute_path.c_str(), flags);
    // }
    // ext4_fopen2(temp_file, absolute_path.c_str());
    return 0;
}

int vfs_is_dir(eastl::string &absolute_path)
{
    struct ext4_dir dir_obj;
    struct ext4_dir *dir = &dir_obj;
    printfRed("dir: %p\n", dir);

    int status = ext4_dir_open(dir, absolute_path.c_str());
    printfYellow("dir->f.mp->name: %s\n", dir->f.mp->name);
    if (status < 0)
    {
        return status;
    }
    // Do something with the directory
    return 0;
}
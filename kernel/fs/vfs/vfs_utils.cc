#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags)
{
    // [[maybe_unused]] struct ext4_file *temp_file = &file->lwext4_file_struct;
    // [[maybe_unused]] int status = -100;

    int type = vfs_path2filetype(absolute_path);
    int status = -100;
    if (type == fs::FileTypes::FT_NORMAL)
    {
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        fs::normal_file *temp_file = new fs::normal_file(attrs, absolute_path);
        status = ext4_fopen2(&temp_file->lwext4_file_struct, absolute_path.c_str(), flags);
        if (status != EOK)
        {
            // vfs_free_file(&temp_file->lwext4_file_struct);
            panic("没写free逻辑");
            return -ENOMEM;
        }
        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DEVICE)
    {
        panic("FT_DEVICE is not supported yet"); //下面写的是错的
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DEVICE;
        fs::device_file *temp_file = new fs::device_file(attrs, absolute_path);
        status = ext4_fopen2(&temp_file->lwext4_file_struct, absolute_path.c_str(), flags);
        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DIRECT)
    {
        panic("FT_DIRECT is not supported yet");
        // 目录
        // struct ext4_dir dir_obj;
        // struct ext4_dir *dir = &dir_obj;
        // status = ext4_dir_open(dir, absolute_path.c_str());
        // if (status < 0)
        // {
        //     return status;
        // }
        // file = dir; // 将目录对象赋值给 file
    }
    else
    {
        panic("Unsupported file type: %d", type);
        return -ENOTSUP;
    }

    return EOK;
}

int vfs_is_dir(eastl::string &absolute_path)
{
    //这个函数可以滚蛋了，以后弃用
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

int vfs_path2filetype(eastl::string &absolute_path)
{
    struct ext4_inode inode;
    uint32 ino;
    if (ext4_raw_inode_fill(absolute_path.c_str(), &ino, &inode) == EOK)
    {
        struct ext4_sblock *sb = NULL;
        ext4_get_sblock(absolute_path.c_str(), &sb);
        int type = ext4_inode_type(sb, &inode);
        if (sb != NULL)
        {
            switch (type)
            {
            case EXT4_INODE_MODE_CHARDEV:
                return fs::FileTypes::FT_DEVICE;
            case EXT4_INODE_MODE_DIRECTORY:
                return fs::FileTypes::FT_DIRECT;
            case EXT4_INODE_MODE_FILE:
                return fs::FileTypes::FT_NORMAL;
            case EXT4_INODE_MODE_SOFTLINK:
                return fs::FileTypes::FT_SYMLINK;
            case EXT4_INODE_MODE_FIFO:
                return fs::FileTypes::FT_PIPE;
            case EXT4_INODE_MODE_SOCKET:
                return fs::FileTypes::FT_DEVICE;
            default:
                panic("一直游到海水变蓝");
            }
        }
    }
    panic("今天是个好日子");
    return -1;
}
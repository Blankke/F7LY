#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/directory_file.hh"
int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags)
{
    if(is_file_exist(absolute_path.c_str()) != 1)
    {
        return -ENOENT; // 文件不存在
    }
    //TODO: 这里flag之类的都没处理，瞎jb open
    printfCyan("[vfs_openat] absolute_path: %s, flags: %x\n", absolute_path.c_str(), flags);
    int type = vfs_path2filetype(absolute_path);
    int status = -100;
    if (type == fs::FileTypes::FT_NORMAL)
    {
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        attrs._value = 0777;
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
        // 创建目录文件对象
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DIRECT;
        attrs._value = 0755; // 目录权限
        
        // 假设你有一个 directory_file 类
        fs::directory_file *temp_dir = new fs::directory_file(attrs, absolute_path);
        
        // 使用 ext4_dir_open 打开目录
        status = ext4_dir_open(&temp_dir->lwext4_dir_struct, absolute_path.c_str());
        if (status != EOK)
        {
            delete temp_dir;
            printfRed("Failed to open directory: %d\n", status);
            return status;
        }
        
        file = temp_dir;
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
                panic("一直游到海水变蓝.");
            }
        }
    }
    printfMagenta("path2filetype: %s not found\n", absolute_path.c_str());
    return -1;
}




int create_and_write_file(const char *path, const char *data)
{
    int res;
    ext4_file file;

    // 检查文件是否已存在
    if (is_file_exist(path) == 1)
    {
        printf("File already exists: %s\n", path);
        ext4_fclose(&file);
        return EEXIST;
    }

    // 创建并打开文件
    res = ext4_fopen(&file, path, "wb+");
    if (res != EOK)
    {
        printf("Failed to open file: %d\n", res);
        return res;
    }

    // 写入数据
    size_t data_len = strlen(data);
    size_t written;
    res = ext4_fwrite(&file, data, data_len, &written);
    if (res != EOK || written != data_len)
    {
        printf("Failed to write file: %d, written: %zu\n", res, written);
        ext4_fclose(&file);
        return res;
    }

    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printf("Failed to close file: %d\n", res);
        return res;
    }

    return EOK;
}

int is_file_exist(const char *path)
{
    struct ext4_inode inode;
    uint32_t ino;
    printfYellow("check file existence: %s\n", path);
    // 尝试获取文件的inode信息
    int res = ext4_raw_inode_fill(path, &ino, &inode);
    //TODO : 这里有个特别诡异的现象，加了print下面这行会爆炸
    // printf("res:%p\n", res);

    if (res == EOK) {
        // 文件存在
        return 1;
    } else if (res == ENOENT) {
        // 文件不存在
        return 0;
    } else {
        // 其他错误（如权限问题、路径错误等）
        return -res;  // 返回负的错误码
    }
}
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size)
{
    // if (is_file_exist(path) != 1)
    // {
    //     printfRed("文件不存在\n");
    //     return -ENOENT;
    // }
    
    int res;
    ext4_file file;
    
    // 打开文件（只读模式）
    res = ext4_fopen(&file, path, "rb");
    if (res != EOK)
    {
        printfRed("Failed to open file: %d\n", res);
        return res;
    }
    
    // 如果有偏移，设置文件指针位置
    if (offset > 0)
    {
        res = ext4_fseek(&file, offset, SEEK_SET);
        if (res != EOK)
        {
            printfRed("Failed to seek file: %d\n", res);
            ext4_fclose(&file);
            return res;
        }
    }
    
    // 读取数据
    size_t bytes_read;
    res = ext4_fread(&file, (void*)buffer_addr, size, &bytes_read);
    if (res != EOK)
    {
        printfRed("Failed to read file: %d\n", res);
        ext4_fclose(&file);
        return res;
    }
    
    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printfRed("Failed to close file: %d\n", res);
        return res;
    }
    
    // 返回实际读取的字节数
    return bytes_read;
}
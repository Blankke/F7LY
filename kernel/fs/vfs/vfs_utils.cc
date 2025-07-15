#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/directory_file.hh"
int vfs_openat(eastl::string absolute_path, fs::file *&file, uint flags)
{
    if (is_file_exist(absolute_path.c_str()) != 1 && (flags & O_CREAT) == 0)
    {
        printfRed("vfs_openat: file %s does not exist, flags: %d\n", absolute_path.c_str(), flags);
        return -ENOENT; // 文件不存在
    }
    // TODO: 这里flag之类的都没处理，瞎jb open
    int type = vfs_path2filetype(absolute_path);
    int status = -100;
    if (type == fs::FileTypes::FT_NORMAL || (flags & O_CREAT) != 0)
    {
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        attrs._value = 0777;
        fs::normal_file *temp_file = new fs::normal_file(attrs, absolute_path);
        printfYellow("flags: %x\n", flags);
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
        panic("FT_DEVICE is not supported yet"); // 下面写的是错的
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
    // 这个函数可以滚蛋了，以后弃用
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
    // TODO : 这里有个特别诡异的现象，加了print下面这行会爆炸
    //  printf("res:%p\n", res);

    if (res == EOK)
    {
        // 文件存在
        return 1;
    }
    else if (res == ENOENT)
    {
        // 文件不存在
        return 0;
    }
    else
    {
        // 其他错误（如权限问题、路径错误等）
        return -res; // 返回负的错误码
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
    res = ext4_fread(&file, (void *)buffer_addr, size, &bytes_read);
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

int vfs_getdents(fs::file *&file, struct linux_dirent64 *dirp, uint count)
{
    int index = 0;
    struct linux_dirent64 *d;
    const ext4_direntry *rentry;
    int totlen = 0;
    uint64 current_offset = 0;

    /* make integer count */
    if (count == 0)
    {
        return -EINVAL;
    }
    ext4_dir_entry_next(&file->lwext4_dir_struct);
    ext4_dir_entry_next(&file->lwext4_dir_struct); //< 跳过/.和/..
    d = dirp;
    while (1)
    {
        rentry = ext4_dir_entry_next(&file->lwext4_dir_struct);
        if (rentry == NULL)
            break;

        int namelen = strlen((const char *)rentry->name);
        /*
         * 长度是前四项的19加上namelen(字符串长度包括结尾的\0)
         * reclen是namelen+2,如果是+1会错误。原因是没考虑name[]开头的'\'
         */
        uint reclen = sizeof d->d_ino + sizeof d->d_off + sizeof d->d_reclen + sizeof d->d_type + namelen + 1;
        if (reclen % 8)
            reclen = reclen - reclen % 8 + 8; //<对齐
        if (reclen < sizeof(struct linux_dirent64))
            reclen = sizeof(struct linux_dirent64);

        if (totlen + reclen >= count)
            break;

        char name[MAXPATH] = {0};
        // name[0] = '/';
        strcat(name, (const char *)rentry->name); //< 追加，二者应该都以'/'开头
        strncpy(d->d_name, name, MAXPATH);

        if (rentry->inode_type == EXT4_DE_DIR)
        {
            d->d_type = T_DIR;
        }
        else if (rentry->inode_type == EXT4_DE_REG_FILE)
        {
            d->d_type = T_FILE;
        }
        else if (rentry->inode_type == EXT4_DE_CHRDEV)
        {
            d->d_type = T_CHR;
        }
        else
        {
            d->d_type = T_UNKNOWN;
        }
        d->d_ino = rentry->inode;
        d->d_off = current_offset + reclen; // start from 1
        d->d_reclen = reclen;
        ++index;
        totlen += d->d_reclen;
        current_offset += reclen;
        d = (struct linux_dirent64 *)((char *)d + d->d_reclen);
    }

    return totlen;
}

int vfs_mkdir(const char *path, uint64_t mode)
{
    /* Create the directory. */
    int status = ext4_dir_mk(path);
    if (status != EOK)
        return -status;

    /* Set mode. */
    status = ext4_mode_set(path, mode);

    return -status;
}

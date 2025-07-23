#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_oflags.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/directory_file.hh"


// 将flags转换为可读的字符串表示
eastl::string flags_to_string(uint flags)
{
    eastl::string result;

    // 处理访问模式（互斥的，只能是其中一个）
    int access_mode = flags & 0x3;
    switch (access_mode)
    {
    case O_RDONLY:
        result += "O_RDONLY";
        break;
    case O_WRONLY:
        result += "O_WRONLY";
        break;
    case O_RDWR:
        result += "O_RDWR";
        break;
    default:
        result += "UNKNOWN_ACCESS";
        break;
    }

    // 处理其他标志（可以组合）
    if (flags & O_CREAT)
        result += "|O_CREAT";
    if (flags & O_EXCL)
        result += "|O_EXCL";
    if (flags & O_NOCTTY)
        result += "|O_NOCTTY";
    if (flags & O_TRUNC)
        result += "|O_TRUNC";
    if (flags & O_APPEND)
        result += "|O_APPEND";
    if (flags & O_NONBLOCK)
        result += "|O_NONBLOCK";
    if (flags & O_DSYNC)
        result += "|O_DSYNC";
    if (flags & O_ASYNC)
        result += "|O_ASYNC";
    if (flags & O_DIRECT)
        result += "|O_DIRECT";
    if (flags & O_LARGEFILE)
        result += "|O_LARGEFILE";
    if (flags & O_DIRECTORY)
        result += "|O_DIRECTORY";
    if (flags & O_NOFOLLOW)
        result += "|O_NOFOLLOW";
    if (flags & O_NOATIME)
        result += "|O_NOATIME";
    if (flags & O_CLOEXEC)
        result += "|O_CLOEXEC";
    if (flags & O_SYNC)
        result += "|O_SYNC";
    if (flags & O_PATH)
        result += "|O_PATH";
    if (flags & O_TMPFILE)
        result += "|O_TMPFILE";

    // 如果有未识别的标志，显示原始十六进制值
    uint known_flags = O_RDWR | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |
                       O_NONBLOCK | O_DSYNC | O_ASYNC | O_DIRECT | O_LARGEFILE |
                       O_DIRECTORY | O_NOFOLLOW | O_NOATIME | O_CLOEXEC | O_SYNC |
                       O_PATH | O_TMPFILE;
    uint unknown_flags = flags & ~known_flags;
    if (unknown_flags)
    {
        printfRed("Unknown flags: 0x%x\n", unknown_flags);
    }

    return result;
}

// 辅助函数：根据flags和文件类型确定文件权限
static mode_t determine_file_mode(uint flags, fs::FileTypes file_type, bool file_exists, int requested_mode)
{
    mode_t mode;

    switch (file_type)
    {
    case fs::FileTypes::FT_NORMAL:
        if (!file_exists && (flags & O_CREAT))
        {
            // 新创建的普通文件，使用请求的权限模式
            mode = requested_mode;
        }
        else
        {
            // 现有的普通文件，保持当前权限（这里给默认值）
            mode = 0644;
        }
        break;

    case fs::FileTypes::FT_DEVICE:
        mode = 0666; // rw-rw-rw-
        break;

    case fs::FileTypes::FT_DIRECT:
        mode = 0755; // rwxr-xr-x
        break;

    default:
        mode = 0644; // 默认权限
        break;
    }

    // 正确处理文件访问模式：检查低两位来确定读写权限
    // 注意：只修改基本权限位（低9位），保留特殊权限位（sticky bit, setuid, setgid）
    mode_t special_bits = mode & 07000;  // 保存特殊权限位（sticky, setuid, setgid）
    mode_t basic_perms = mode & 0777;    // 获取基本权限位
    
    int access_mode = flags & 0x3; // 取低两位
    if (access_mode == O_RDONLY)   // 0x00 - 只读
    {
        basic_perms &= ~0222; // 清除写权限
        basic_perms |= 0444;  // 设置读权限
    }
    else if (access_mode == O_WRONLY) // 0x01 - 只写
    {
        basic_perms &= ~0444; // 清除读权限
        basic_perms |= 0222; // 设置写权限
    }
    // O_RDWR (0x02) 保持读写权限不变
    
    // 合并特殊权限位和基本权限位
    mode = special_bits | basic_perms;

    return mode;
}
int vfs_openat(eastl::string absolute_path, fs::file *&file, uint flags, int mode)
{
    bool file_exists = (vfs_is_file_exist(absolute_path.c_str()) == 1);
    // 好多flag都有人给你负重前行过了

    // 处理 O_DIRECTORY：如果指定了此标志，路径必须是目录
    if (flags & O_DIRECTORY)
    {
        int type = vfs_path2filetype(absolute_path);
        if (file_exists && type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_DIRECTORY specified but %s is not a directory\n", absolute_path.c_str());
            return -ENOTDIR; // 不是目录
        }
    }

    // 处理 O_NOFOLLOW：如果路径的最后一个组件是符号链接，则失败
    if (flags & O_NOFOLLOW)
    {
        int type = vfs_path2filetype(absolute_path);
        if (file_exists && type == fs::FileTypes::FT_SYMLINK)
        {
            printfRed("vfs_openat: O_NOFOLLOW specified but %s is a symbolic link\n", absolute_path.c_str());
            return -ELOOP; // 表示符号链接过多
        }
    }

    // 处理 O_EXCL + O_CREAT 组合：如果文件存在，应该失败
    if ((flags & O_CREAT) && (flags & O_EXCL) && file_exists)
    {
        printfRed("vfs_openat: file %s already exists with O_CREAT|O_EXCL\n", absolute_path.c_str());
        return -EEXIST;
    }

    // 如果文件不存在且没有O_CREAT标志，返回错误
    if (!file_exists && (flags & O_CREAT) == 0)
    {
        printfRed("vfs_openat: file %s does not exist, flags: %d\n", absolute_path.c_str(), flags);
        return -ENOENT; // 文件不存在
    }

    int type = vfs_path2filetype(absolute_path);
    int status = -100;

    if (type == fs::FileTypes::FT_NORMAL || (flags & O_CREAT) != 0)
    {
        // 根据flags和文件类型确定适当的权限
        // 专门重写了个函数来确定这个权限
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_NORMAL, file_exists, mode);

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        attrs._value = file_mode;

        fs::normal_file *temp_file = new fs::normal_file(attrs, absolute_path);
        printfYellow("vfs_openat: flags: %x, mode: %b\n", flags, temp_file->_attrs.transMode());

        // ext4库会自动处理 O_TRUNC, O_RDONLY, O_WRONLY, O_RDWR 等标志
        // 真是前人栽树，后人乘凉啊！
        status = ext4_fopen2(&temp_file->lwext4_file_struct, absolute_path.c_str(), flags);
        if (status != EOK)
        {
            delete temp_file;
            printfRed("ext4_fopen2 failed with status: %d\n", status);
            return -ENOMEM;
        }

        // 如果是新创建的文件，设置文件权限到 ext4 inode
        if (!file_exists && (flags & O_CREAT))
        {
            status = ext4_mode_set(absolute_path.c_str(), file_mode);
            if (status != EOK)
            {
                printfRed("ext4_mode_set failed for %s, status: %d\n", absolute_path.c_str(), status);
                // 不返回错误，因为文件已经创建成功了
            }
            else
            {
                printfGreen("ext4_mode_set success for %s, mode: %o\n", absolute_path.c_str(), file_mode);
            }
        }

        // 处理 O_APPEND：将文件指针设置到文件末尾
        if (flags & O_APPEND)
        {
            // 这是纯sb设计，后面有机会把这个删了
            temp_file->setAppend();
        }

        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DEVICE)
    {
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_DEVICE, file_exists, mode);

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DEVICE;
        attrs._value = file_mode;

        fs::device_file *temp_file = new fs::device_file(attrs, absolute_path);
        status = ext4_fopen2(&temp_file->lwext4_file_struct, absolute_path.c_str(), flags);
        if (status != EOK)
        {
            delete temp_file;
            printfRed("Failed to open device file: %d\n", status);
            return status;
        }
        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_DIRECT)
    {
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_DIRECT, file_exists, mode);

        // 创建目录文件对象
        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_DIRECT;
        attrs._value = file_mode;

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

    // 处理 O_LARGEFILE：检查文件大小限制
    if (!(flags & O_LARGEFILE) && file != nullptr)
    {
        // 对于32位系统，如果文件大小超过2GB且没有指定O_LARGEFILE，应该失败
        // 这里简化处理，假设如果文件存在且大小超过限制就报错
        if (file_exists && file->_stat.size > 0x7FFFFFFF) // 2GB
        {
            printfRed("vfs_openat: file %s is too large, O_LARGEFILE required\n", absolute_path.c_str());
            delete file;
            file = nullptr;
            return -EOVERFLOW;
        }
    }

    // 处理 O_CLOEXEC：设置执行时关闭标志
    if ((flags & O_CLOEXEC) && file != nullptr)
    {
        // 在文件对象上设置相应的标志
        // 这个标志会在exec系统调用时自动关闭文件描述符
        // 注意：这里需要在实际使用时在文件描述符表中设置FD_CLOEXEC
        printfYellow("vfs_openat: O_CLOEXEC flag set for file %s\n", absolute_path.c_str());
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
    // printfMagenta("path2filetype: %s not found\n", absolute_path.c_str());
    return -1;
}

int create_and_write_file(const char *path, const char *data)
{
    int res;
    ext4_file file;

    // 检查文件是否已存在
    if (vfs_is_file_exist(path) == 1)
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
        printf("Failed to write file: %d, written: %u\n", res, written);
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

int vfs_is_file_exist(const char *path)
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
    // if (vfs_is_file_exist(path) != 1)
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

int vfs_getdents(fs::file *const file, struct linux_dirent64 *dirp, uint count)
{
    int index = 0;
    struct linux_dirent64 *d;
    const ext4_direntry *rentry;
    int totlen = 0;
    uint64 current_offset = 0;

    /* make integer count */
    if (count == 0)
    {
        return EINVAL;
    }
    if (file == nullptr || file->lwext4_dir_struct.f.mp == nullptr)
    {
        printfRed("[vfs_getdents] file is null or mount point is null\n");
        return EINVAL;
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

int vfs_fstat(fs::file *f, fs::Kstat *st)
{
    struct ext4_inode inode;
    uint32 inode_num = 0;
    const char *file_path = f->_path_name.c_str();

    int status = ext4_raw_inode_fill(file_path, &inode_num, &inode);
    if (status != EOK)
        return -status;

    struct ext4_sblock *sb = NULL;
    status = ext4_get_sblock(file_path, &sb);
    if (status != EOK)
        return -status;

    st->dev = 0;
    st->ino = inode_num;
    st->mode = ext4_inode_get_mode(sb, &inode);
    st->nlink = ext4_inode_get_links_cnt(&inode);
    st->uid = ext4_inode_get_uid(&inode);
    st->gid = ext4_inode_get_gid(&inode);
    st->rdev = ext4_inode_get_dev(&inode);
    st->size = inode.size_lo;
    st->blksize = inode.size_lo / inode.blocks_count_lo;
    st->blocks = (uint64)inode.blocks_count_lo;

    st->st_atime_sec = ext4_inode_get_access_time(&inode);
    st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
    st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
    st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    return EOK;
}

int vfs_frename(const char *oldpath, const char *newpath)
{
    int status = ext4_frename(oldpath, newpath);
    if (status != EOK)
        return -status;

    return -status;
}

int vfs_truncate(fs::file *f, size_t length)
{
    if (f == nullptr)
    {
        printfRed("vfs_truncate: file is null\n");
        return -EINVAL;
    }

    // 直接调用ext4的truncate函数
    int status = ext4_ftruncate(&f->lwext4_file_struct, length);
    if (status != EOK)
    {
        printfRed("vfs_truncate: failed to truncate file %s, error: %d\n", f->_path_name.c_str(), status);
        return -status;
    }

    // 更新文件大小
    f->_stat.size = length;

    return EOK;
}
int vfs_chmod(eastl::string pathname, mode_t mode)
{

    if (vfs_is_file_exist(pathname.c_str()) != 1)
    {
        printfRed("[vfs_chmod] 文件不存在: %s\n", pathname.c_str());
        return -ENOENT; // 文件不存在
    }

    // 调用ext4的模式设置函数
    int status = ext4_mode_set(pathname.c_str(), mode);
    if (status != EOK)
    {
        printfRed("[vfs_chmod] 设置文件权限失败: %s, 错误码: %d\n", pathname.c_str(), status);
        return -EACCES; // 访问被拒绝
    }

    return EOK;
}

int vfs_fallocate(fs::file *f, off_t offset, size_t length)
{
    if (f == nullptr)
    {
        printfRed("vfs_fallocate: file is null\n");
        return -EINVAL;
    }

    // 检查参数合法性
    if (offset < 0 || length <= 0)
    {
        printfRed("vfs_fallocate: invalid offset or length\n");
        return -EINVAL;
    }

    // 获取当前文件大小
    uint64_t current_size = ext4_fsize(&f->lwext4_file_struct);
    uint64_t target_size = offset + length;
    if (target_size > EXT4_MAX_FILE_SIZE)
    {
        printfRed("vfs_fallocate: target size exceeds maximum file size\n");
        return -EFBIG; // 文件过大
    }
    // 如果目标大小小于等于当前大小，不需要分配空间
    if (target_size <= current_size)
    {
        return EOK;
    }

    // 使用 ext4_ftruncate 来扩展文件大小
    // 这会自动分配必要的磁盘块
    int status = ext4_ftruncate(&f->lwext4_file_struct, target_size);
    if (status != EOK)
    {
        printfRed("vfs_fallocate: failed to allocate space for file %s, error: %d\n",
                  f->_path_name.c_str(), status);
        return status;
    }

    // 更新文件大小信息
    f->_stat.size = target_size;

    printfGreen("vfs_fallocate: successfully allocated space for file %s, new size: %u\n",
                f->_path_name.c_str(), target_size);

    return EOK;
}

int vfs_free_file(fs::file *file)
{
    ///@todo 锁
    delete file;
    return 0;
}
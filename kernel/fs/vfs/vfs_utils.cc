#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_oflags.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "fs/vfs/file/directory_file.hh"
#include "fs/vfs/fifo_manager.hh"
#include "proc_manager.hh" // 用于访问当前进程的umask
#include "fs/lwext4/ext4.hh"
#include <EASTL/vector.h>

// 解析符号链接路径
static int resolve_symlinks(const eastl::string &input_path, eastl::string &resolved_path, int max_depth = 8)
{
    if (max_depth <= 0)
    {
        return -ELOOP; // 符号链接嵌套太深
    }

    resolved_path = input_path;

    // 按 '/' 分割路径
    eastl::vector<eastl::string> path_parts;
    eastl::string current_part;

    for (size_t i = 0; i < input_path.length(); i++)
    {
        if (input_path[i] == '/')
        {
            if (!current_part.empty())
            {
                path_parts.push_back(current_part);
                current_part.clear();
            }
        }
        else
        {
            current_part += input_path[i];
        }
    }
    if (!current_part.empty())
    {
        path_parts.push_back(current_part);
    }

    // 重新构建路径，逐步检查每个组件是否为符号链接
    eastl::string current_path = "/";

    for (size_t i = 0; i < path_parts.size(); i++)
    {
        if (current_path.back() != '/')
        {
            current_path += "/";
        }
        current_path += path_parts[i];
        // printfYellow("Checking path component: %s\n", current_path.c_str());
        // 检查当前路径是否为符号链接
        int type = vfs_path2filetype(current_path);
        if (type == fs::FileTypes::FT_SYMLINK)
        {
            // 读取符号链接内容
            char link_target[256];
            size_t link_len;
            int r = ext4_readlink(current_path.c_str(), link_target, sizeof(link_target) - 1, &link_len);
            if (r != EOK)
            {
                return -ENOENT;
            }
            link_target[link_len] = '\0';

            eastl::string link_path(link_target);

            eastl::string new_path;

            // 如果符号链接是绝对路径，重新开始
            if (link_path[0] == '/')
            {
                new_path = link_path;
            }
            else
            {
                // 相对路径：需要相对于当前组件的父目录
                size_t last_slash = current_path.find_last_of('/');
                if (last_slash == eastl::string::npos || last_slash == 0)
                {
                    new_path = "/" + link_path;
                }
                else
                {
                    new_path = current_path.substr(0, last_slash + 1) + link_path;
                }
            }

            // 添加剩余的路径组件
            for (size_t j = i + 1; j < path_parts.size(); j++)
            {
                if (new_path.back() != '/')
                {
                    new_path += "/";
                }
                new_path += path_parts[j];
            }

            printfYellow("Resolving symlink %s -> %s, final path: %s\n",
                         current_path.c_str(), link_path.c_str(), new_path.c_str());

            // 递归解析剩余的符号链接
            return resolve_symlinks(new_path, resolved_path, max_depth - 1);
        }
    }

    resolved_path = current_path;
    return 0;
}

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

// 辅助函数：应用进程的umask到权限模式
static mode_t apply_umask(mode_t mode)
{
    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        // 如果无法获取当前进程，使用默认umask 022
        return mode & ~0022;
    }

    // 应用当前进程的umask：从mode中清除umask中设置的权限位
    return mode & ~(current_proc->_umask);
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
            // 新创建的普通文件，使用请求的权限模式并应用umask
            mode = apply_umask(requested_mode);
        }
        else
        {
            // 现有的普通文件，保持当前权限（这里给默认值）
            mode = 0644;
        }
        break;

    case fs::FileTypes::FT_DEVICE:
        mode = 0666; // rw-rw-rw-，设备文件通常不应用umask
        break;

    case fs::FileTypes::FT_PIPE:
        // FIFO/管道文件，使用默认权限并应用umask
        mode = apply_umask(0644); // rw-r--r--
        break;

    case fs::FileTypes::FT_DIRECT:
        if (!file_exists)
        {
            // 新创建的目录，应用umask
            mode = apply_umask(0755); // rwxr-xr-x
        }
        else
        {
            mode = 0755; // 现有目录保持原权限
        }
        break;

    default:
        mode = apply_umask(0644); // 默认权限并应用umask
        break;
    }

    // 文件权限应该由创建时的mode参数决定，而不是由打开标志决定
    // 文件的访问标志(O_RDONLY, O_WRONLY, O_RDWR)只影响打开时的读写权限，
    // 不应该修改文件本身的权限位

    return mode;
}
int vfs_openat(eastl::string absolute_path, fs::file *&file, uint flags, int mode)
{
    // printfYellow("[vfs_openat] : absolute_path=%s, flags=%o, mode=0%o\n", absolute_path.c_str(), flags, mode);

    bool file_exists = (vfs_is_file_exist(absolute_path.c_str()) == 1);

    // 处理 O_EXCL + O_CREAT 组合：如果文件存在，应该失败
    if ((flags & O_CREAT) && (flags & O_EXCL) && file_exists)
    {
        printfRed("vfs_openat: file %s already exists with O_CREAT|O_EXCL\n", absolute_path.c_str());
        return -EEXIST;
    }

    // 处理 O_TMPFILE：创建匿名临时文件
    if (flags & O_TMPFILE)
    {
        // 去除末尾斜杠
        eastl::string dir_path = absolute_path;
        if (!dir_path.empty() && dir_path.back() == '/')
            dir_path.pop_back();

        // O_TMPFILE 要求路径必须是一个存在的目录
        if (!file_exists)
        {
            printfRed("vfs_openat: O_TMPFILE specified but directory %s does not exist\n", dir_path.c_str());
            return -ENOENT;
        }

        int dir_type = vfs_path2filetype(dir_path);
        if (dir_type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_TMPFILE specified but %s is not a directory\n", dir_path.c_str());
            return -ENOTDIR;
        }

        // O_TMPFILE 的两种情况处理
        int access_mode = flags & O_ACCMODE;
        if (access_mode == O_RDONLY)
        {
            // O_TMPFILE | O_RDONLY：打开目录进行读取，而不是创建临时文件
            printfGreen("vfs_openat: O_TMPFILE|O_RDONLY - opening directory %s for reading\n", dir_path.c_str());
            
            // 移除 O_TMPFILE 标志，按普通目录打开处理
            flags &= ~O_TMPFILE;
            absolute_path = dir_path; // 使用处理过的路径（去除末尾斜杠）
            // 继续执行下面的普通文件处理逻辑
        }
        else
        {
            // O_TMPFILE | O_RDWR/O_WRONLY：创建匿名临时文件
            printfGreen("vfs_openat: O_TMPFILE with write access - creating anonymous temporary file\n");
            
            // 创建匿名临时文件 - 使用静态计数器和进程地址生成唯一路径
            static uint64_t tmp_counter = 0;
            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            uint64_t unique_id = ++tmp_counter + (uint64_t)current_proc;

            char tmp_name[256];
            snprintf(tmp_name, sizeof(tmp_name), "%s/.tmpfile_%x",
                     dir_path.c_str(), unique_id);

            eastl::string tmp_path(tmp_name);

            // 创建临时文件（移除 O_DIRECTORY 和 O_TMPFILE 标志）
            uint temp_flags = flags & ~(O_DIRECTORY | O_TMPFILE);
            temp_flags |= O_CREAT | O_EXCL; // 确保创建新文件

            mode_t file_mode = determine_file_mode(temp_flags, fs::FileTypes::FT_NORMAL, false, mode);

            fs::FileAttrs attrs;
            attrs.filetype = fs::FileTypes::FT_NORMAL;
            attrs._value = file_mode;

            fs::normal_file *temp_file = new fs::normal_file(attrs, tmp_path);

            // 创建临时文件
            int status = ext4_fopen2(&temp_file->lwext4_file_struct, tmp_path.c_str(), temp_flags);
            if (status != EOK)
            {
                delete temp_file;
                printfRed("vfs_openat: failed to create O_TMPFILE: %d\n", status);
                return -status; // 返回正确的错误码
            }

            // 重要：恢复 O_TMPFILE 标志，以便权限检查时能识别这是一个临时文件
            temp_file->lwext4_file_struct.flags |= O_TMPFILE;

            // // 立即从目录中删除文件条目，使其成为匿名文件
            // // 这样文件就只能通过文件描述符访问，实现真正的O_TMPFILE语义
            // int unlink_status = ext4_fremove(tmp_path.c_str());
            // if (unlink_status != EOK)
            // {
            //     printfRed("vfs_openat: warning - failed to unlink O_TMPFILE: %d\n", unlink_status);
            //     // 不返回错误，因为文件已经创建成功
            // }

            // 设置文件权限
            status = ext4_mode_set(tmp_path.c_str(), file_mode);
            if (status != EOK)
            {
                printfGreen("vfs_openat: ext4_mode_set skipped for O_TMPFILE\n");
                // 对于临时文件，这是正常的
            }

            printfGreen("vfs_openat: created O_TMPFILE file, mode: 0%o\n", file_mode);

            file = temp_file;
            return EOK;
        }
    }

    // 如果文件不存在且没有O_CREAT标志，返回错误
    if (!file_exists && (flags & O_CREAT) == 0)
    {
        printfRed("vfs_openat: file %s does not exist, flags: %d\n", absolute_path.c_str(), flags);
        return -ENOENT; // 文件不存在
    }

    // 确定要使用的实际路径和文件类型
    eastl::string actual_path = absolute_path;
    int type = -1;

    if (file_exists)
    {
        type = vfs_path2filetype(absolute_path);
    }
    else
    {
        type = fs::FileTypes::FT_NORMAL; // 新文件默认为普通文件
    }

    // 处理 O_DIRECTORY：如果指定了此标志，路径必须是目录
    if ((flags & O_DIRECTORY))
    {
        if (file_exists && type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_DIRECTORY specified but %s is not a directory\n", absolute_path.c_str());
            return -ENOTDIR; // 不是目录
        }
    }

    // 处理符号链接
    if (type == fs::FileTypes::FT_SYMLINK)
    {

        if (flags & O_NOFOLLOW)
        {
            // 如果指定了 O_NOFOLLOW，我们需要创建一个符号链接文件对象
            // 这样 fstat 可以获取符号链接本身的属性
            printfYellow("vfs_openat: O_NOFOLLOW specified, creating symlink file object for %s\n", absolute_path.c_str());

            // 创建一个普通文件对象来表示符号链接
            // 但是文件类型标记为符号链接
            fs::FileAttrs attrs;
            attrs.filetype = fs::FileTypes::FT_SYMLINK;
            attrs._value = 0777; // 符号链接通常有全权限

            fs::normal_file *temp_file = new fs::normal_file(attrs, absolute_path);
            // 不需要调用 ext4_fopen2，因为我们只是要获取符号链接的属性
            // 直接设置状态即可
            file = temp_file;
            return EOK;
        }
        else
        {
            // 如果没有指定 O_NOFOLLOW，解析符号链接
            eastl::string resolved_path;
            int r = resolve_symlinks(absolute_path, resolved_path);
            if (r < 0)
            {
                printfRed("vfs_openat: failed to resolve symlink %s, error: %d\n", absolute_path.c_str(), r);
                return r;
            }

            actual_path = resolved_path;
            printfYellow("vfs_openat: resolved symlink %s -> %s\n", absolute_path.c_str(), resolved_path.c_str());

            // 重新检查解析后路径的存在性和类型
            file_exists = (vfs_is_file_exist(actual_path.c_str()) == 1);
            if (file_exists)
            {
                type = vfs_path2filetype(actual_path);
            }
            else
            {
                type = fs::FileTypes::FT_NORMAL; // 默认为普通文件（可能需要创建）
            }
        }
    }
    int status = -100;

    if (type == fs::FileTypes::FT_NORMAL || (flags & O_CREAT) != 0)
    {
        // 根据flags和文件类型确定适当的权限
        // 专门重写了个函数来确定这个权限
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_NORMAL, file_exists, mode);

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_NORMAL;
        attrs._value = file_mode;

        fs::normal_file *temp_file = new fs::normal_file(attrs, actual_path);
        // printfYellow("vfs_openat: flags: %o, mode: 0%o, actual_path: %s\n", flags, temp_file->_attrs.transMode(), actual_path.c_str());

        // ext4库会自动处理 O_TRUNC, O_RDONLY, O_WRONLY, O_RDWR 等标志
        // 真是前人栽树，后人乘凉啊！
        status = ext4_fopen2(&temp_file->lwext4_file_struct, actual_path.c_str(), flags);
        if (status != EOK)
        {
            delete temp_file;
            printfRed("ext4_fopen2 failed with status: %d for path: %s\n", status, actual_path.c_str());
            return -ENOMEM;
        }

        // 如果是新创建的文件，设置文件权限到 ext4 inode
        bool is_newly_created = !file_exists && (flags & O_CREAT);
        if (is_newly_created)
        {
            status = ext4_mode_set(actual_path.c_str(), file_mode);
            if (status != EOK)
            {
                printfRed("ext4_mode_set failed for %s, status: %d\n", actual_path.c_str(), status);
                // 不返回错误，因为文件已经创建成功了
            }
            else
            {
                printfGreen("ext4_mode_set success for %s, mode: 0%o\n", actual_path.c_str(), file_mode);
            }

            // 设置文件所有者和组
            // 获取当前进程的 uid 和 gid
            proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
            uint32_t current_uid = 1; // 使用与 sys_getuid() 一致的值
            uint32_t current_gid = 1; // 使用与 sys_getgid() 一致的值

            if (current_proc != nullptr)
            {
                // 注意：虽然进程的_uid可能是0，但为了与sys_getuid()保持一致，
                // 文件创建时应使用sys_getuid()返回的值（即1）
                current_uid = 1; // 与 sys_getuid() 返回值保持一致
                current_gid = 1; // 与 sys_getgid() 返回值保持一致
            }

            // 设置文件的 uid 和 gid
            status = ext4_owner_set(actual_path.c_str(), current_uid, current_gid);
            if (status != EOK)
            {
                printfRed("ext4_owner_set failed for %s, status: %d\n", actual_path.c_str(), status);
            }
            else
            {
                printfGreen("ext4_owner_set success for %s, uid: %u, gid: %u\n",
                            actual_path.c_str(), current_uid, current_gid);
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

        fs::device_file *temp_file = new fs::device_file(attrs, actual_path);
        status = ext4_fopen2(&temp_file->lwext4_file_struct, actual_path.c_str(), flags);
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

        fs::directory_file *temp_dir = new fs::directory_file(attrs, actual_path);

        // 使用 ext4_dir_open 打开目录
        status = ext4_dir_open(&temp_dir->lwext4_dir_struct, actual_path.c_str());
        if (status != EOK)
        {
            delete temp_dir;
            printfRed("Failed to open directory: %d\n", status);
            return status;
        }

        file = temp_dir;
    }
    else if (type == fs::FileTypes::FT_PIPE)
    {
        mode_t file_mode;

        if (file_exists)
        {
            // 如果文件已存在，从文件系统读取权限
            struct ext4_inode inode;
            uint32 ino;
            if (ext4_raw_inode_fill(absolute_path.c_str(), &ino, &inode) == EOK)
            {
                struct ext4_sblock *sb = NULL;
                ext4_get_sblock(absolute_path.c_str(), &sb);
                if (sb != NULL)
                {
                    file_mode = ext4_inode_get_mode(sb, &inode);
                }
                else
                {
                    file_mode = 0644; // 默认权限
                }
            }
            else
            {
                file_mode = 0644; // 默认权限
            }
        }
        else
        {
            file_mode = determine_file_mode(flags, fs::FileTypes::FT_PIPE, file_exists, mode);
        }

        // 根据打开模式确定是读端还是写端
        int access_mode = flags & O_ACCMODE;

        // 检查 O_NONBLOCK | O_WRONLY 的组合
        // 根据 Linux manual，当使用 O_NONBLOCK | O_WRONLY 打开 FIFO 时，
        // 如果没有进程打开该 FIFO 进行读取，应该返回 ENXIO 错误
        if ((flags & O_NONBLOCK) && (access_mode == O_WRONLY))
        {
            // 检查是否有其他进程已经打开了这个 FIFO 进行读取
            if (!fs::k_fifo_manager.has_readers(absolute_path))
            {
                printfRed("vfs_openat: O_NONBLOCK | O_WRONLY on FIFO %s with no readers\n", absolute_path.c_str());
                return -ENXIO; // 没有设备或地址
            }
        }

        fs::FileAttrs attrs;
        attrs.filetype = fs::FileTypes::FT_PIPE;
        attrs._value = file_mode & 0777; // 只保留权限位

        // 对于 FIFO，使用全局管理器获取或创建 Pipe 对象
        proc::ipc::Pipe *pipe = fs::k_fifo_manager.get_or_create_fifo(absolute_path);

        bool is_write_end = false;
        if (access_mode == O_WRONLY)
        {
            is_write_end = true;
        }
        else if (access_mode == O_RDONLY)
        {
            is_write_end = false;
        }
        else
        {
            // O_RDWR - 这种情况下我们默认创建读端，实际应用中可能需要特殊处理
            is_write_end = false;
        }

        // 创建带有路径信息的 pipe_file
        fs::pipe_file *temp_file = new fs::pipe_file(attrs, pipe, is_write_end, absolute_path);

        // 注册到全局管理器
        fs::k_fifo_manager.open_fifo(absolute_path, is_write_end);

        printfCyan("vfs_openat: Created FIFO/pipe file: %s, write_end: %d, mode: 0%o\n",
                   absolute_path.c_str(), is_write_end, temp_file->_stat.mode);
        status = EOK; // 直接设置为成功

        file = temp_file;
    }
    else if (type == fs::FileTypes::FT_SOCKET)
    {
        // Socket文件暂时不支持
        printfRed("vfs_openat: O_SOCKET not supported yet\n");
        return -ENOSYS; // Socket文件暂时不支持
    }
    else
    {
        printfRed("Unsupported file type: %d\n", type);
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
    // printfYellow("vfs_is_file_exist: checking path: %s\n", path);
    // 尝试获取文件的inode信息
    int res = ext4_raw_inode_fill(path, &ino, &inode);
    // printfYellow("vfs_is_file_exist: ext4_raw_inode_fill returned: %d for path: %s\n", res, path);
    // TODO : 这里有个特别诡异的现象，加了print下面这行会爆炸
    //  printf("res:%p\n", res);

    if (res == EOK)
    {
        // 文件存在
        // printfGreen("vfs_is_file_exist: file exists: %s\n", path);
        return 1;
    }
    else if (res == ENOENT)
    {
        // 文件不存在
        printfRed("vfs_is_file_exist: file not found: %s\n", path);
        return 0;
    }
    else
    {
        // 其他错误（如权限问题、路径错误等）
        printfRed("vfs_is_file_exist: error %d for path: %s\n", res, path);
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
        
        // 过滤掉 O_TMPFILE 创建的临时文件，让它们在目录遍历时不可见
        if (strncmp(name, ".tmpfile_", 9) == 0) {
            printfYellow("vfs_getdents: filtering out O_TMPFILE: %s\n", name);
            continue; // 跳过这个条目，不返回给用户空间
        }
        
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

    /* Apply umask to the mode and set directory permissions. */
    mode_t final_mode = apply_umask(mode);
    status = ext4_mode_set(path, final_mode);

    return -status;
}

int vfs_fstat(fs::file *f, fs::Kstat *st)
{
    // 检查是否是 pipe_file，如果是，直接使用其内部的 _stat
    if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
    {
        *st = f->_stat;
        printfCyan("vfs_fstat: pipe file, mode: 0%o\n", st->mode);
        return EOK;
    }

    // 检查是否是符号链接文件
    if (f->_attrs.filetype == fs::FileTypes::FT_SYMLINK)
    {
        printfCyan("vfs_fstat: symlink file, getting symlink attributes\n");

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

        // 获取原始 uid 和 gid
        st->uid = ext4_inode_get_uid(&inode);
        st->gid = ext4_inode_get_gid(&inode);

        st->rdev = ext4_inode_get_dev(&inode);
        st->size = inode.size_lo; // 符号链接的大小是目标路径的长度
        st->blksize = 4096;
        st->blocks = (st->size + 511) / 512;

        st->st_atime_sec = ext4_inode_get_access_time(&inode);
        st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF;
        st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
        st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF;
        st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
        st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF;
        st->mnt_id = 0;

        printfCyan("vfs_fstat: symlink mode: 0%o, size: %u\n", st->mode, st->size);
        return EOK;
    }
    struct ext4_inode inode;
    uint32 inode_num = 0;
    const char *file_path = f->_path_name.c_str();
    int status = ext4_raw_inode_fill(file_path, &inode_num, &inode);
    if (status != EOK)
    {
        printfRed("vfs_fstat: ext4_raw_inode_fill failed for %s, error: %d\n", file_path, status);
        return -status;
    }
    struct ext4_sblock *sb = NULL;
    status = ext4_get_sblock(file_path, &sb);
    if (status != EOK)
        return -status;

    st->dev = 0;
    st->ino = inode_num;
    st->mode = ext4_inode_get_mode(sb, &inode);
    st->nlink = ext4_inode_get_links_cnt(&inode);

    // 获取原始 uid 和 gid，进行范围检查
    uint32_t raw_uid = ext4_inode_get_uid(&inode);
    uint32_t raw_gid = ext4_inode_get_gid(&inode);

    // 检查是否有异常值，如果有则使用默认值
    if (raw_uid > 65535)
    {                // 超出合理范围
        st->uid = 0; // 默认 root
        printfRed("vfs_fstat: invalid uid %u, using 0\n", raw_uid);
    }
    else
    {
        st->uid = raw_uid;
    }

    if (raw_gid > 65535)
    {                // 超出合理范围
        st->gid = 0; // 默认 root group
        printfRed("vfs_fstat: invalid gid %u, using 0\n", raw_gid);
    }
    else
    {
        st->gid = raw_gid;
    }

    st->rdev = ext4_inode_get_dev(&inode);
    st->size = inode.size_lo;
    printfCyan("vfs_fstat: file size: %u bytes\n", st->size);
    // 修复 blksize 计算：避免除零错误，使用标准块大小
    st->blksize = 4096; // 使用标准 4KB 块大小

    // 修复 blocks 计算：根据文件大小计算所需的512字节块数
    // Linux stat 中的 blocks 字段表示分配给文件的512字节块数
    // 对于小文件，通常文件大小决定块数
    if (st->size == 0)
    {
        st->blocks = 0;
    }
    else
    {
        // 计算所需的512字节块数，向上取整
        st->blocks = (st->size + 511) / 512;

        // 但是要考虑文件系统的实际分配情况
        // 如果 ext4 报告的块数更小且合理，使用它
        uint64 ext4_blocks_512 = ((uint64)inode.blocks_count_lo * 4096) / 512;
        if (ext4_blocks_512 > 0 && ext4_blocks_512 < st->blocks)
        {
            st->blocks = ext4_blocks_512;
        }

        // 确保至少有1个块（对于非空文件）
        if (st->blocks == 0 && st->size > 0)
        {
            st->blocks = 1;
        }
    }

    st->st_atime_sec = ext4_inode_get_access_time(&inode);
    st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
    st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
    st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF; //< 30 bits for nanoseconds
    st->mnt_id = 0;                                            // ext4暂时不支持挂载点ID
    return EOK;
}

int vfs_frename(const char *oldpath, const char *newpath)
{
    int status = ext4_frename(oldpath, newpath);
    if (status != EOK)
        return -status;

    return -status;
}

int vfs_link(const char *oldpath, const char *newpath)
{
    printfYellow("vfs_link: checking source file existence: %s\n", oldpath);
    
    // 检查源文件是否存在
    int file_exists = vfs_is_file_exist(oldpath);
    printfYellow("vfs_link: vfs_is_file_exist returned: %d for path: %s\n", file_exists, oldpath);
    
    if (file_exists != 1)
    {
        printfRed("vfs_link: source file %s does not exist\n", oldpath);
        return -ENOENT;
    }

    // 检查目标文件是否已存在
    if (vfs_is_file_exist(newpath) == 1)
    {
        printfRed("vfs_link: target file %s already exists\n", newpath);
        return -EEXIST;
    }

    // 检查源文件是否为目录
    eastl::string old_path_str(oldpath);
    int source_type = vfs_path2filetype(old_path_str);
    if (source_type == fs::FileTypes::FT_DIRECT)
    {
        printfRed("vfs_link: cannot create hard link to directory %s\n", oldpath);
        return -EPERM;
    }

    // 使用 ext4_flink 创建硬链接
    int status = ext4_flink(oldpath, newpath);
    if (status != EOK)
    {
        printfRed("vfs_link: ext4_flink failed for %s -> %s, error: %d\n",
                  oldpath, newpath, status);
        return -status;
    }

    printfGreen("vfs_link: successfully created hard link %s -> %s\n", newpath, oldpath);
    return EOK;
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


bool is_lock_conflict(const struct flock &existing_lock, const struct flock &new_lock)
{
    printfCyan("[is_lock_conflict] Existing: type=%d, start=%ld, len=%ld, pid=%d\n", 
              existing_lock.l_type, existing_lock.l_start, existing_lock.l_len, existing_lock.l_pid);
    printfCyan("[is_lock_conflict] New: type=%d, start=%ld, len=%ld, pid=%d\n", 
              new_lock.l_type, new_lock.l_start, new_lock.l_len, new_lock.l_pid);

    // 如果现有锁类型是F_UNLCK或目标锁类型是解锁（F_UNLCK），就不需要检测冲突
    if (existing_lock.l_type == 2 || new_lock.l_type == 2) // F_UNLCK = 2
    {
        printfCyan("[is_lock_conflict] One lock is F_UNLCK, no conflict\n");
        return false;
    }

    // 判断锁的范围是否重叠
    off_t start1 = existing_lock.l_start;
    off_t end1 = (existing_lock.l_len == 0) ? LONG_MAX : existing_lock.l_start + existing_lock.l_len;
    off_t start2 = new_lock.l_start;
    off_t end2 = (new_lock.l_len == 0) ? LONG_MAX : new_lock.l_start + new_lock.l_len;

    printfCyan("[is_lock_conflict] Range check: [%ld,%ld] vs [%ld,%ld]\n", start1, end1, start2, end2);

    // 如果锁的范围没有交集，直接返回不冲突
    if (end1 <= start2 || end2 <= start1)
    {
        printfCyan("[is_lock_conflict] No range overlap, no conflict\n");
        return false;
    }

    // 如果是同一个进程的锁，不冲突（进程可以修改自己的锁）
    if (existing_lock.l_pid == new_lock.l_pid && existing_lock.l_pid != 0)
    {
        printfCyan("[is_lock_conflict] Same process, no conflict\n");
        return false;
    }

    // 检查锁类型是否冲突
    if (existing_lock.l_type == 1 || new_lock.l_type == 1) // F_WRLCK = 1
    {
        printfCyan("[is_lock_conflict] Write lock involved, conflict!\n");
        return true; // 写锁和任何锁都冲突
    }
    if (existing_lock.l_type == 0 && new_lock.l_type == 0) // F_RDLCK = 0
    {
        printfCyan("[is_lock_conflict] Both read locks, no conflict\n");
        return false; // 读锁之间不冲突
    }
    
    printfCyan("[is_lock_conflict] Other case, conflict!\n");
    return true;      // 其他情况下有冲突
}

// 检查文件锁是否允许指定的读写操作
bool check_file_lock_access(const struct flock &file_lock, off_t offset, size_t size, bool is_write)
{
    // 如果文件没有锁，允许任何操作
    if (file_lock.l_type == F_UNLCK)
        return true;

    // 计算操作的范围
    off_t op_start = offset;
    off_t op_end = offset + size;

    // 计算锁的范围
    off_t lock_start = file_lock.l_start;
    off_t lock_end = (file_lock.l_len == 0) ? LONG_MAX : file_lock.l_start + file_lock.l_len;

    // 如果操作范围与锁范围没有交集，允许操作
    if (op_end <= lock_start || lock_end <= op_start)
        return true;

    // 如果有交集，检查锁类型
    if (file_lock.l_type == F_WRLCK)
        return false; // 写锁阻止任何操作

    if (file_lock.l_type == F_RDLCK && is_write)
        return false; // 读锁阻止写操作

    return true; // 读锁允许读操作
}
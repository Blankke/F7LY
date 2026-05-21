#include "vfs_utils.hh"
#include "fs/vfs/fs.hh"
#include "fs/lwext4/ext4_errno.hh"
#include "fs/lwext4/ext4_inode.hh"
#include "fs/lwext4/ext4_oflags.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/file/device_file.hh"
#include "fs/vfs/file/pipe_file.hh"
#include "fs/vfs/file/directory_file.hh"
#include "fs/vfs/file/fat32_file.hh"
#include "fs/fat32/fat32.hh"
#include "fs/vfs/fifo_manager.hh"
#include "fs/vfs/virtual_fs.hh"
#include "proc_manager.hh" // 用于访问当前进程的umask
#include "fs/lwext4/ext4.hh"
#include "fs/vfs/vfs_ext4_ext.hh" // 包含 NS_to_S 宏
#include "tm/time.h"              // 包含 TIME2NS 宏
#include "mem/memlayout.hh"
#include <EASTL/vector.h>
#include <EASTL/algorithm.h>
#include <libs/string.hh>

namespace
{
#ifdef RISCV
    constexpr uint64 k_min_kernel_file_ptr = KERNBASE;
#elif defined(LOONGARCH)
    constexpr uint64 k_min_kernel_file_ptr = PHYSBASE;
#endif
    constexpr size_t k_linux_path_max = 4096;
    constexpr size_t k_linux_name_max = 255;

    struct MountOverride
    {
        eastl::string path;
        bool read_only = false;
    };

    eastl::vector<MountOverride> g_mount_overrides;

    bool path_matches_mount_prefix(const eastl::string &path, const eastl::string &mount_path)
    {
        if (mount_path.empty())
        {
            return false;
        }

        if (mount_path == "/")
        {
            return !path.empty() && path[0] == '/';
        }

        if (path.compare(0, mount_path.size(), mount_path) != 0)
        {
            return false;
        }

        return path.size() == mount_path.size() || path[mount_path.size()] == '/';
    }

    const MountOverride *find_mount_override(const eastl::string &path)
    {
        const MountOverride *best = nullptr;

        for (const auto &entry : g_mount_overrides)
        {
            if (!path_matches_mount_prefix(path, entry.path))
            {
                continue;
            }

            if (best == nullptr || entry.path.size() > best->path.size())
            {
                best = &entry;
            }
        }

        return best;
    }

    bool open_wants_write_access(uint flags)
    {
        int access_mode = flags & O_ACCMODE;
        return access_mode != O_RDONLY ||
               (flags & (O_CREAT | O_TRUNC | O_TMPFILE)) != 0;
    }

    int validate_linux_path_length(const eastl::string &path)
    {
        if (path.length() >= k_linux_path_max)
        {
            return -ENAMETOOLONG;
        }

        size_t component_len = 0;
        for (size_t i = 0; i < path.length(); ++i)
        {
            if (path[i] == '/')
            {
                component_len = 0;
                continue;
            }

            ++component_len;
            if (component_len > k_linux_name_max)
            {
                return -ENAMETOOLONG;
            }
        }

        return EOK;
    }

    int required_open_access_mask(uint flags)
    {
        int access_mask = 0;
        switch (flags & O_ACCMODE)
        {
        case O_WRONLY:
            access_mask |= W_OK;
            break;
        case O_RDWR:
            access_mask |= (R_OK | W_OK);
            break;
        case O_RDONLY:
        default:
            access_mask |= R_OK;
            break;
        }

        if (flags & O_TRUNC)
        {
            access_mask |= W_OK;
        }

        return access_mask;
    }

    int check_mode_bits_with_fsids(uint32 mode, uint32 owner_uid, uint32 owner_gid,
                                   uint32 fsuid, uint32 fsgid, int requested_mask)
    {
        if (fsuid == 0)
        {
            return 0;
        }

        uint32 perms = 0;
        if (fsuid == owner_uid)
        {
            perms = (mode >> 6) & 0x7;
        }
        else if (fsgid == owner_gid)
        {
            perms = (mode >> 3) & 0x7;
        }
        else
        {
            perms = mode & 0x7;
        }

        if ((requested_mask & R_OK) && !(perms & 0x4))
            return -EACCES;
        if ((requested_mask & W_OK) && !(perms & 0x2))
            return -EACCES;
        if ((requested_mask & X_OK) && !(perms & 0x1))
            return -EACCES;
        return 0;
    }

    int validate_existing_open_permissions(const eastl::string &absolute_path, uint flags)
    {
        proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
        if (current_proc == nullptr)
        {
            return -EFAULT;
        }

        fs::Kstat st;
        int stat_ret = vfs_path_stat(absolute_path.c_str(), &st, true);
        if (stat_ret < 0)
        {
            return stat_ret;
        }

        return check_mode_bits_with_fsids(st.mode,
                                          st.uid,
                                          st.gid,
                                          current_proc->get_fsuid(),
                                          current_proc->get_fsgid(),
                                          required_open_access_mask(flags));
    }

    inline bool is_kernel_mapped_file_range(uint64 addr, uint64 size)
    {
        if (addr < k_min_kernel_file_ptr || size == 0)
        {
            return false;
        }

        uint64 end = addr + size - 1;
        if (end < addr)
        {
            return false;
        }

        return mem::k_pagetable.kwalk_addr(addr) != 0 &&
               mem::k_pagetable.kwalk_addr(end) != 0;
    }

    inline bool is_probably_live_file_object(fs::file *file_obj)
    {
        if (file_obj == nullptr)
        {
            return false;
        }

        if (!is_kernel_mapped_file_range((uint64)file_obj, sizeof(fs::file)))
        {
            return false;
        }

        uint64 vtable_addr = *(uint64 *)file_obj;
        return is_kernel_mapped_file_range(vtable_addr, sizeof(void *));
    }

    inline bool path_equals_or_has_child(const eastl::string &path, const char *prefix)
    {
        if (prefix == nullptr)
        {
            return false;
        }

        size_t prefix_len = strlen(prefix);
        if (path.length() < prefix_len || path.compare(0, prefix_len, prefix) != 0)
        {
            return false;
        }

        return path.length() == prefix_len || path[prefix_len] == '/';
    }

    bool remap_glibc_runtime_path(const eastl::string &path, eastl::string &remapped_path)
    {
        struct PrefixAlias
        {
            const char *from;
            const char *to;
        };

        static const PrefixAlias k_prefix_aliases[] = {
            {"/lib/riscv64-linux-gnu", "/glibc/lib"},
            {"/lib/loongarch64-linux-gnu", "/glibc/lib"},
            {"/usr/lib/riscv64-linux-gnu", "/glibc/lib"},
            {"/usr/lib/loongarch64-linux-gnu", "/glibc/lib"},
            {"/lib64", "/glibc/lib"},
            {"/usr/lib64", "/glibc/lib"},
        };

        static const PrefixAlias k_exact_aliases[] = {
            {"/lib/ld-linux-riscv64-lp64d.so.1", "/glibc/lib/ld-linux-riscv64-lp64d.so.1"},
            {"/lib64/ld-linux-riscv64-lp64d.so.1", "/glibc/lib/ld-linux-riscv64-lp64d.so.1"},
            {"/lib/ld-linux-loongarch-lp64d.so.1", "/glibc/lib/ld-linux-loongarch-lp64d.so.1"},
            {"/lib64/ld-linux-loongarch-lp64d.so.1", "/glibc/lib/ld-linux-loongarch-lp64d.so.1"},
        };

        for (const auto &alias : k_exact_aliases)
        {
            if (path == alias.from)
            {
                remapped_path = alias.to;
                return true;
            }
        }

        for (const auto &alias : k_prefix_aliases)
        {
            if (!path_equals_or_has_child(path, alias.from))
            {
                continue;
            }

            remapped_path = alias.to;
            if (path.length() > strlen(alias.from))
            {
                remapped_path += path.c_str() + strlen(alias.from);
            }
            return true;
        }

        return false;
    }

    eastl::string get_parent_path(const eastl::string &path)
    {
        size_t last_slash = path.find_last_of('/');
        if (last_slash == eastl::string::npos)
        {
            return ".";
        }
        if (last_slash == 0)
        {
            return "/";
        }
        return path.substr(0, last_slash);
    }

    int raw_vfs_is_file_exist(const eastl::string &path)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (Printer::trace_group_enabled())
                tracef("[raw_vfs_is_file_exist] fat32 path=%s rel=%s entry=%p\n",
                       path.c_str(), rel_path, ep);
            if (ep != nullptr)
            {
                eput(ep);
                return 1;
            }
            return 0;
        }

        struct ext4_inode inode;
        uint32_t ino;
        int res = ext4_raw_inode_fill(path.c_str(), &ino, &inode);
        if (res == EOK)
        {
            return 1;
        }
        if (res == ENOENT)
        {
            return 0;
        }
        return -res;
    }

    int raw_vfs_path2filetype(const eastl::string &path)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (!ep)
                return -1;
            int type = fs::FileTypes::FT_NORMAL;
            if (ep->attribute & ATTR_DIRECTORY)
                type = fs::FileTypes::FT_DIRECT;
            eput(ep);
            return type;
        }

        struct ext4_inode inode;
        uint32 ino;
        if (ext4_raw_inode_fill(path.c_str(), &ino, &inode) != EOK)
        {
            return -1;
        }

        struct ext4_sblock *sb = NULL;
        ext4_get_sblock(path.c_str(), &sb);
        if (sb == NULL)
        {
            return -1;
        }

        switch (ext4_inode_type(sb, &inode))
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
            printfRed("raw_vfs_path2filetype: unknown file type for path: %s\n", path.c_str());
            return -1;
        }
    }

    int raw_vfs_path_stat(const eastl::string &path, fs::Kstat *st)
    {
        struct filesystem *fs = get_fs_from_path(path.c_str());
        if (fs && fs->type == FAT32)
        {
            const char *rel_path = path.c_str();
            if (strcmp(fs->path, "/") != 0)
            {
                size_t mplen = strlen(fs->path);
                if (strncmp(rel_path, fs->path, mplen) == 0)
                {
                    if (rel_path[mplen] == '\0')
                        rel_path = "/";
                    else if (rel_path[mplen] == '/')
                        rel_path += mplen;
                }
            }
            struct fat32_entry *ep = ename((char *)rel_path);
            if (!ep)
                return -ENOENT;

            memset(st, 0, sizeof(fs::Kstat));
            st->ino = (uint64)ep;
            st->dev = fs->dev;
            st->mode = 0;
            if (ep->attribute & ATTR_DIRECTORY)
                st->mode |= S_IFDIR;
            else
                st->mode |= S_IFREG;

            st->mode |= 0755;
            if (ep->attribute & ATTR_READ_ONLY)
                st->mode &= ~0222;

            st->nlink = 1;
            st->uid = 0;
            st->gid = 0;
            st->size = ep->file_size;

            struct mntfs *mnt = (struct mntfs *)fs->fs_data;
            st->blksize = mnt ? mnt->byts_per_clus : 4096;
            st->blocks = (st->size + 511) / 512;

            eput(ep);
            return EOK;
        }

        struct ext4_inode inode;
        uint32 inode_num = 0;
        int status = ext4_raw_inode_fill(path.c_str(), &inode_num, &inode);
        if (status != EOK)
        {
            printfRed("vfs_path_stat: ext4_raw_inode_fill failed for %s, error: %d\n",
                      path.c_str(), status);
            return -status;
        }

        struct ext4_sblock *sb = NULL;
        status = ext4_get_sblock(path.c_str(), &sb);
        if (status != EOK)
        {
            return -status;
        }

        st->dev = 0;
        st->ino = inode_num;
        st->mode = ext4_inode_get_mode(sb, &inode);
        st->nlink = ext4_inode_get_links_cnt(&inode);

        uint32_t raw_uid = ext4_inode_get_uid(&inode);
        uint32_t raw_gid = ext4_inode_get_gid(&inode);
        if (raw_uid > 65535)
        {
            st->uid = 0;
            printfRed("vfs_path_stat: invalid uid %u, using 0\n", raw_uid);
        }
        else
        {
            st->uid = raw_uid;
        }

        if (raw_gid > 65535)
        {
            st->gid = 0;
            printfRed("vfs_path_stat: invalid gid %u, using 0\n", raw_gid);
        }
        else
        {
            st->gid = raw_gid;
        }

        st->rdev = ext4_inode_get_dev(&inode);
        st->size = inode.size_lo;
        st->blksize = 4096;
        if (st->size == 0)
        {
            st->blocks = 0;
        }
        else
        {
            st->blocks = (st->size + 511) / 512;
            if (st->blocks == 0 && st->size > 0)
            {
                st->blocks = 1;
            }
        }

        st->st_atime_sec = ext4_inode_get_access_time(&inode);
        st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF;
        st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
        st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF;
        st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
        st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF;
        st->mnt_id = 0;

        return EOK;
    }

    bool select_runtime_alias_path(const eastl::string &requested_path,
                                   eastl::string &selected_path,
                                   bool allow_parent_fallback)
    {
        selected_path = requested_path;

        eastl::string remapped_path;
        if (!remap_glibc_runtime_path(requested_path, remapped_path) || remapped_path == requested_path)
        {
            return false;
        }

        if (raw_vfs_is_file_exist(requested_path) == 1)
        {
            return false;
        }

        if (raw_vfs_is_file_exist(remapped_path) == 1)
        {
            selected_path = remapped_path;
            return true;
        }

        if (!allow_parent_fallback)
        {
            return false;
        }

        eastl::string requested_parent = get_parent_path(requested_path);
        eastl::string remapped_parent = get_parent_path(remapped_path);
        if (raw_vfs_is_file_exist(requested_parent) != 1 &&
            raw_vfs_is_file_exist(remapped_parent) == 1)
        {
            selected_path = remapped_path;
            return true;
        }

        return false;
    }
}

// 路径规范化函数：处理 . 和 ..
eastl::string normalize_path(const eastl::string &path)
{
    if (path.empty())
        return path;

    eastl::vector<eastl::string> components;
    eastl::string current_component;
    bool is_absolute = (path[0] == '/');

    // 分割路径组件
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
        {
            if (!current_component.empty())
            {
                components.push_back(current_component);
                current_component.clear();
            }
        }
        else
        {
            current_component += path[i];
        }
    }
    if (!current_component.empty())
    {
        components.push_back(current_component);
    }

    // 处理 . 和 ..
    eastl::vector<eastl::string> normalized;
    for (const auto &comp : components)
    {
        if (comp == ".")
        {
            // 忽略当前目录
            continue;
        }
        else if (comp == "..")
        {
            // 上级目录
            if (!normalized.empty() && normalized.back() != "..")
            {
                normalized.pop_back();
            }
            else if (!is_absolute)
            {
                // 对于相对路径，保留 ..
                normalized.push_back(comp);
            }
            // 对于绝对路径，根目录的上级还是根目录，所以忽略
        }
        else
        {
            normalized.push_back(comp);
        }
    }

    // 重建路径
    eastl::string result;
    if (is_absolute)
    {
        result = "/";
    }

    for (size_t i = 0; i < normalized.size(); ++i)
    {
        if (i > 0 || is_absolute)
        {
            if (result.back() != '/')
                result += "/";
        }
        result += normalized[i];
    }

    // 如果结果为空且是绝对路径，返回根目录
    if (result.empty() && is_absolute)
    {
        result = "/";
    }
    // 如果结果为空且是相对路径，返回当前目录
    else if (result.empty())
    {
        result = ".";
    }

    return result;
}

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

            // 标准化路径，处理 . 和 .. 组件
            new_path = normalize_path(new_path);

            // printfYellow("Resolving symlink %s -> %s, final path: %s\n",
            //              current_path.c_str(), link_path.c_str(), new_path.c_str());

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

    // 正确处理文件访问模式：检查低两位来确定读写权限
    // 注意：只修改基本权限位（低9位），保留特殊权限位（sticky bit, setuid, setgid）
    mode_t special_bits = mode & 07000; // 保存特殊权限位（sticky, setuid, setgid）
    mode_t basic_perms = mode & 0777;   // 获取基本权限位

    int access_mode = flags & 0x3; // 取低两位
    if (access_mode == O_RDONLY)   // 0x00 - 只读
    {
        basic_perms &= ~0222; // 清除写权限
        basic_perms |= 0444;  // 设置读权限
    }
    else if (access_mode == O_WRONLY) // 0x01 - 只写
    {
        basic_perms &= ~0444; // 清除读权限
        basic_perms |= 0222;  // 设置写权限
    }
    else if (access_mode == O_RDWR) // 0x02 - 读写
    {
        basic_perms |= 0444; // 设置读权限
        basic_perms |= 0222; // 设置写权限
    }

    // 合并特殊权限位和基本权限位
    mode = special_bits | basic_perms;

    return mode;
}

// 新建 inode 的权限应该只来源于调用者传入的 mode 与 umask，
// 不能被这次 open() 的 O_RDONLY/O_WRONLY/O_RDWR 访问模式污染。
static mode_t determine_created_inode_mode(fs::FileTypes file_type, int requested_mode)
{
    switch (file_type)
    {
    case fs::FileTypes::FT_NORMAL:
        return apply_umask(requested_mode);
    case fs::FileTypes::FT_PIPE:
        return apply_umask(0644);
    case fs::FileTypes::FT_DIRECT:
        return apply_umask(0755);
    case fs::FileTypes::FT_DEVICE:
        return 0666;
    default:
        return apply_umask(requested_mode);
    }
}

static int validate_lookup_prefix_permissions(const eastl::string &absolute_path)
{
    if (absolute_path.empty() || absolute_path[0] != '/')
    {
        return EOK;
    }

    size_t last_slash = absolute_path.find_last_of('/');
    if (last_slash == eastl::string::npos || last_slash == 0)
    {
        return EOK;
    }

    proc::Pcb *current_proc = proc::k_pm.get_cur_pcb();
    if (current_proc == nullptr)
    {
        return -EFAULT;
    }

    eastl::string parent_path = absolute_path.substr(0, last_slash);

    for (size_t start = 1; start < parent_path.length();)
    {
        size_t end = parent_path.find('/', start);
        if (end == eastl::string::npos)
            end = parent_path.length();

        eastl::string current_path = parent_path.substr(0, end);

        int exists = vfs_is_file_exist(current_path.c_str());
        if (exists < 0)
            return exists;
        if (exists == 0)
            return -ENOENT;

        fs::Kstat st;
        int stat_ret = vfs_path_stat(current_path.c_str(), &st, true);
        if (stat_ret < 0)
            return stat_ret;
        if ((st.mode & S_IFMT) != S_IFDIR)
            return -ENOTDIR;

        uint32_t fsuid = current_proc->get_fsuid();
        uint32_t fsgid = current_proc->get_fsgid();
        if (fsuid != 0)
        {
            bool has_search_permission = false;
            if (fsuid == st.uid)
                has_search_permission = (st.mode & S_IXUSR) != 0;
            else if (fsgid == st.gid)
                has_search_permission = (st.mode & S_IXGRP) != 0;
            else
                has_search_permission = (st.mode & S_IXOTH) != 0;

            if (!has_search_permission)
                return -EACCES;
        }

        start = end + 1;
    }

    return EOK;
}

int vfs_openat(eastl::string absolute_path, fs::file *&file, uint flags, int mode)
{
    // printfYellow("[vfs_openat] : absolute_path=%s, flags=%o, mode=0%o\n", absolute_path.c_str(), flags, mode);
    int length_ret = validate_linux_path_length(absolute_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_openat: path too long: len=%u\n", (uint32)absolute_path.length());
        return length_ret;
    }

    // 解析路径中的符号链接
    eastl::string resolved_path = absolute_path;

    // 分离父目录和文件名
    eastl::string parent_dir;
    eastl::string filename;

    size_t last_slash = absolute_path.find_last_of('/');
    if (last_slash != eastl::string::npos && last_slash > 0)
    {
        parent_dir = absolute_path.substr(0, last_slash);
        filename = absolute_path.substr(last_slash + 1);
    }
    else
    {
        parent_dir = "/";
        if (last_slash == 0 && absolute_path.length() > 1)
            filename = absolute_path.substr(1);
        else
            filename = absolute_path;
    }

    // 总是解析父目录中的符号链接
    eastl::string resolved_parent;
    int r = resolve_symlinks(parent_dir, resolved_parent);
    if (r < 0)
    {
        printfRed("vfs_openat: failed to resolve parent path %s, error: %d\n", parent_dir.c_str(), r);
        // 只有在严重错误时才返回，否则继续使用原路径
        if (r == -ELOOP)
            return r;
        resolved_parent = parent_dir; // 使用原路径
    }

    // 重新构建路径
    resolved_path = resolved_parent;
    if (resolved_path.back() != '/')
        resolved_path += "/";
    resolved_path += filename;
    length_ret = validate_linux_path_length(resolved_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_openat: resolved path too long: len=%u\n", (uint32)resolved_path.length());
        return length_ret;
    }

    // 检查是否为 FAT32 分区
    struct filesystem *fs = get_fs_from_path(resolved_path.c_str());
    if (fs && fs->type == FAT32) {
         const char* rel_path = resolved_path.c_str();
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         
         if (!ep && (flags & O_CREAT)) {
             // Create file
             // Need to find parent entry first
             char name_buf[FAT32_MAX_FILENAME + 1];
             struct fat32_entry *dp = enameparent((char*)rel_path, name_buf);
             if (dp) {
                 elock(dp);
                 int attr = 0;
                 if (flags & O_DIRECTORY) attr = ATTR_DIRECTORY;
                 ep = ealloc(dp, name_buf, attr);
                 eunlock(dp);
                 eput(dp);
             }
         }
         
         if (ep) {
             // Check directory if O_DIRECTORY set
             if ((flags & O_DIRECTORY) && !(ep->attribute & ATTR_DIRECTORY)) {
                  eput(ep);
                  return -ENOTDIR;
             }
             
             // Create file object
             fs::FileAttrs attrs;
             if (ep->attribute & ATTR_DIRECTORY) attrs.filetype = fs::FileTypes::FT_DIRECT;
             else attrs.filetype = fs::FileTypes::FT_NORMAL;
             
             // Determine mode (simplified for FAT32)
             mode_t file_mode = determine_file_mode(flags, attrs.filetype, true, mode);
             attrs._value = file_mode;
             
             file = new fs::fat32_file(attrs, resolved_path, ep);
             
             // Handle O_TRUNC
             if ((flags & O_TRUNC) && !(ep->attribute & ATTR_DIRECTORY)) {
                 elock(ep);
                 etrunc(ep);
                 eunlock(ep);
             }
             
             // Handle O_APPEND
             if ((flags & O_APPEND) && !(ep->attribute & ATTR_DIRECTORY)) {
                  file->lseek(0, SEEK_END);
             }

             return EOK;
         } else {
             return -ENOENT;
         }
    }

    // 如果没有 O_NOFOLLOW 标志，还要解析最终的文件名（如果它是符号链接）
    if (!(flags & O_NOFOLLOW))
    {
        r = resolve_symlinks(resolved_path, resolved_path);
        if (r < 0)
        {
            printfRed("vfs_openat: failed to resolve final path %s, error: %d\n", resolved_path.c_str(), r);
            if (r == -ELOOP)
                return r;
            // 对于其他错误，继续使用当前路径
        }
    }

    eastl::string lookup_path = resolved_path;
    select_runtime_alias_path(resolved_path, lookup_path, true);

    int prefix_permission_ret = validate_lookup_prefix_permissions(lookup_path);
    if (prefix_permission_ret < 0)
    {
        printfRed("vfs_openat: prefix permission/path check failed for %s, error: %d\n",
                  lookup_path.c_str(), prefix_permission_ret);
        return prefix_permission_ret;
    }

    bool file_exists = (vfs_is_file_exist(lookup_path.c_str()) == 1);

    if (open_wants_write_access(flags) && vfs_is_readonly_path(lookup_path))
    {
        printfRed("vfs_openat: readonly mount rejects write-like open for %s\n", lookup_path.c_str());
        return -EROFS;
    }

    // 处理 O_EXCL + O_CREAT 组合：如果文件存在，应该失败
    if ((flags & O_CREAT) && (flags & O_EXCL) && file_exists)
    {
        printfRed("vfs_openat: file %s already exists with O_CREAT|O_EXCL\n", lookup_path.c_str());
        return -EEXIST;
    }

    if (file_exists)
    {
        int perm_ret = validate_existing_open_permissions(lookup_path, flags);
        if (perm_ret < 0)
        {
            printfRed("vfs_openat: permission denied for %s, flags=%s, err=%d\n",
                      lookup_path.c_str(), flags_to_string(flags).c_str(), perm_ret);
            return perm_ret;
        }
    }

    // 处理 O_TMPFILE：创建匿名临时文件
    if (flags & O_TMPFILE)
    {
        // 去除末尾斜杠
        eastl::string dir_path = resolved_path;
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
            resolved_path = dir_path; // 使用处理过的路径（去除末尾斜杠）
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
            mode_t inode_mode = determine_created_inode_mode(fs::FileTypes::FT_NORMAL, mode);

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
            status = ext4_mode_set(tmp_path.c_str(), inode_mode);
            if (status != EOK)
            {
                printfGreen("vfs_openat: ext4_mode_set skipped for O_TMPFILE\n");
                // 对于临时文件，这是正常的
            }

            printfGreen("vfs_openat: created O_TMPFILE file, mode: 0%o\n", inode_mode);

            file = temp_file;
            return EOK;
        }
    }

    // 如果文件不存在且没有O_CREAT标志，返回错误
    if (!file_exists && (flags & O_CREAT) == 0)
    {
        printfRed("vfs_openat: file %s does not exist, flags: %d\n", lookup_path.c_str(), flags);
        return -ENOENT; // 文件不存在
    }

    // 确定要使用的实际路径和文件类型
    eastl::string actual_path = lookup_path;
    int type = -1;

    if (file_exists)
    {
        type = vfs_path2filetype(actual_path);
    }
    else
    {
        type = fs::FileTypes::FT_NORMAL; // 新文件默认为普通文件
    }

    // 处理 O_NOFOLLOW：如果最终路径是符号链接，应该返回错误
    if ((flags & O_NOFOLLOW) && file_exists && type == fs::FileTypes::FT_SYMLINK)
    {
        printfRed("vfs_openat: O_NOFOLLOW specified but %s is a symlink\n", resolved_path.c_str());
        return -ELOOP; // 符号链接循环错误
    }

    // 处理 O_DIRECTORY：如果指定了此标志，路径必须是目录
    if ((flags & O_DIRECTORY))
    {
        if (file_exists && type != fs::FileTypes::FT_DIRECT)
        {
            printfRed("vfs_openat: O_DIRECTORY specified but %s is not a directory\n", resolved_path.c_str());
            return -ENOTDIR; // 不是目录
        }
    }

    // 处理目录的特殊限制
    if (file_exists && type == fs::FileTypes::FT_DIRECT)
    {
        int access_mode = flags & O_ACCMODE;

        // 目录只能用 O_RDONLY 打开
        if (access_mode != O_RDONLY)
        {
            printfRed("vfs_openat: cannot open directory %s with write access (flags: %s)\n",
                      resolved_path.c_str(), flags_to_string(flags).c_str());
            return -EISDIR; // 是目录错误
        }

        // 不能对已存在的目录使用 O_CREAT
        if (flags & O_CREAT)
        {
            printfRed("vfs_openat: cannot use O_CREAT on existing directory %s\n", resolved_path.c_str());
            return -EISDIR; // 是目录错误
        }
    }

    // 注意：符号链接处理已经在函数开头完成
    // 如果到这里 type 还是 FT_SYMLINK，说明有 O_NOFOLLOW 标志
    // 但我们已经在上面检查过了，所以这里不应该再有符号链接

    int status = -100;

    if (type == fs::FileTypes::FT_NORMAL || (flags & O_CREAT) != 0)
    {
        // 根据flags和文件类型确定适当的权限
        // 专门重写了个函数来确定这个权限
        mode_t file_mode = determine_file_mode(flags, fs::FileTypes::FT_NORMAL, file_exists, mode);
        mode_t inode_mode = determine_created_inode_mode(fs::FileTypes::FT_NORMAL, mode);

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
            return -status;
        }

        // 如果是新创建的文件，设置文件权限到 ext4 inode
        bool is_newly_created = !file_exists && (flags & O_CREAT);
        if (is_newly_created)
        {
            status = ext4_mode_set(actual_path.c_str(), inode_mode);
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
            uint32_t current_uid = 0; // 默认值
            uint32_t current_gid = 0; // 默认值

            if (current_proc != nullptr)
            {
                // Linux 文件系统相关操作应使用 fsuid/fsgid，而不是 euid/egid。
                // 这样 setfsuid/setfsgid、setreuid/setregid 之后的访问控制才会正确。
                current_uid = current_proc->get_fsuid();
                current_gid = current_proc->get_fsgid();
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
        if (flags & O_NONBLOCK)
        {
            pipe->set_nonblock(true);
            printfYellow("vfs_openat: O_NONBLOCK set for FIFO %s\n", absolute_path.c_str());
        }
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
            // O_RDWR 需要像 Linux FIFO 一样同时保留读端和写端，
            // 否则同一个 fd 上的写入会被误判成 EBADF。
            is_write_end = false;
        }

        // 创建带有路径信息的 pipe_file
        fs::pipe_file *temp_file = new fs::pipe_file(attrs, pipe, is_write_end, absolute_path);

        // 注册到全局管理器
        if (access_mode == O_RDWR)
        {
            temp_file->set_duplex_mode();
            fs::k_fifo_manager.open_fifo(absolute_path, false);
            fs::k_fifo_manager.open_fifo(absolute_path, true);
        }
        else
        {
            fs::k_fifo_manager.open_fifo(absolute_path, is_write_end);
        }

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
    eastl::string lookup_path = absolute_path;
    select_runtime_alias_path(absolute_path, lookup_path, false);
    return raw_vfs_path2filetype(lookup_path);
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
    // 先解析符号链接
    eastl::string resolved_path;
    int res = resolve_symlinks(eastl::string(path), resolved_path);
    if (res != 0) {
        printfRed("vfs_is_file_exist: failed to resolve symlinks for path: %s\n", path);
        return 0;
    }

    int exists = raw_vfs_is_file_exist(resolved_path);
    if (exists == 1)
    {
        return 1;
    }

    eastl::string remapped_path;
    if (exists == 0 &&
        remap_glibc_runtime_path(resolved_path, remapped_path) &&
        remapped_path != resolved_path)
    {
        exists = raw_vfs_is_file_exist(remapped_path);
        if (exists == 1)
        {
            return 1;
        }
    }

    if (exists == 0)
    {
        printfRed("vfs_is_file_exist: file not found: %s\n", path);
        return 0;
    }

    printfRed("vfs_is_file_exist: error %d for path: %s\n", -exists, path);
    return exists;
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
    
    // 解析符号链接
    eastl::string resolved_path;
    res = resolve_symlinks(eastl::string(path), resolved_path);
    if (res != 0) {
        printfRed("Failed to resolve symlinks for path: %s, error: %d\n", path, res);
        return res;
    }

    select_runtime_alias_path(resolved_path, resolved_path, false);
    
    struct filesystem *fs = get_fs_from_path(resolved_path.c_str());
    if (fs && fs->type == FAT32) {
         const char* rel_path = resolved_path.c_str();
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         if (!ep) return -ENOENT;
         
         elock(ep);
         int n = eread(ep, 0, buffer_addr, offset, size);
         eunlock(ep);
         eput(ep);
         return n;
    }

    // printfCyan("vfs_read_file: resolved path %s -> %s\n", path, resolved_path.c_str());

    // 打开文件（只读模式）
    res = ext4_fopen(&file, resolved_path.c_str(), "rb");
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
    if (file && file->is_virtual && file->_attrs.filetype == fs::FileTypes::FT_DIRECT)
    {
        eastl::vector<eastl::string> entries;
        entries.push_back(".");
        entries.push_back("..");

        eastl::vector<eastl::string> virtual_children;
        fs::k_vfs.list_virtual_files(file->_path_name, virtual_children);
        for (const auto &name : virtual_children)
        {
            if (eastl::find(entries.begin(), entries.end(), name) == entries.end())
            {
                entries.push_back(name);
            }
        }

        // /proc 需要暴露当前活跃 PID 目录，否则 ps/top 一类工具看不到进程。
        if (file->_path_name == "/proc")
        {
            for (const proc::Pcb &pcb : proc::k_proc_pool)
            {
                if (pcb._state == proc::ProcState::UNUSED || pcb._pid <= 0)
                    continue;

                char pid_buf[16];
                snprintf(pid_buf, sizeof(pid_buf), "%d", pcb._pid);
                eastl::string pid_name(pid_buf);
                if (eastl::find(entries.begin(), entries.end(), pid_name) == entries.end())
                {
                    entries.push_back(pid_name);
                }
            }
        }

        size_t index = static_cast<size_t>(file->_file_ptr);
        struct linux_dirent64 *d = dirp;
        int totlen = 0;

        while (index < entries.size())
        {
            const eastl::string &name = entries[index];
            uint reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + name.size() + 1;
            if (reclen % 8)
                reclen = reclen - reclen % 8 + 8;
            if (reclen < sizeof(struct linux_dirent64))
                reclen = sizeof(struct linux_dirent64);
            if (totlen + (int)reclen > (int)count)
                break;

            memset(d, 0, reclen);
            strncpy(d->d_name, name.c_str(), name.size() + 1);
            d->d_ino = index + 1;
            d->d_off = index + 1;
            d->d_reclen = reclen;

            if (name == "." || name == "..")
            {
                d->d_type = T_DIR;
            }
            else if (file->_path_name == "/proc" && name.size() > 0 && name[0] >= '0' && name[0] <= '9')
            {
                d->d_type = T_DIR;
            }
            else
            {
                eastl::string child_path = file->_path_name;
                if (child_path.empty() || child_path.back() != '/')
                    child_path += "/";
                child_path += name;

                fs::vfile_tree_node *node = fs::k_vfs.get_virtual_node(child_path);
                if (node && node->file_type == fs::FileTypes::FT_DIRECT)
                    d->d_type = T_DIR;
                else
                    d->d_type = T_FILE;
            }

            totlen += reclen;
            d = (struct linux_dirent64 *)((char *)d + reclen);
            ++index;
        }

        file->_file_ptr = index;
        return totlen;
    }

    // FAT32 support
    if (file && file->_attrs.filetype == fs::FileTypes::FT_DIRECT) {
        struct filesystem *fs = get_fs_from_path(file->_path_name.c_str());
        if (fs && fs->type == FAT32) {
             fs::fat32_file *fat_f = (fs::fat32_file*)file;
             if (!fat_f || !fat_f->fat_info.entry) return -EINVAL;
             
             struct fat32_entry *dp = fat_f->fat_info.entry;
             elock(dp);
             
             [[maybe_unused]]int index = 0;
             struct linux_dirent64 *d = dirp;
             int totlen = 0;
             uint32 off = file->_file_ptr; // Use file pointer as offset in directory entries

             struct fat32_entry ep_store;
             struct fat32_entry *ep = &ep_store;
             int ent_count = 0;
             int type;
             
             while (totlen + sizeof(struct linux_dirent64) <= count) {
                  ep->valid = 0;
                  type = enext(dp, ep, off, &ent_count);
                  
                  if (type == -1) {
                      break; // End of directory
                  }
                  
                  off += ent_count * 32; // Advance offset by consumed 32-byte entries
                  
                  if (type == 0) { // Empty entry
                       continue;
                  }

                  int namelen = strlen(ep->filename);
                  uint reclen = sizeof(d->d_ino) + sizeof(d->d_off) + sizeof(d->d_reclen) + sizeof(d->d_type) + namelen + 1;
                  if (reclen % 8) reclen = reclen - reclen % 8 + 8;
                  if (reclen < sizeof(struct linux_dirent64)) reclen = sizeof(struct linux_dirent64);
                  
                  if (totlen + reclen > count) {
                      off -= ent_count * 32;
                      break; 
                  }
                  
                  strncpy(d->d_name, ep->filename, namelen + 1);
                  d->d_ino = 0; 
                  d->d_off = off;
                  d->d_reclen = reclen;
                  
                  if (ep->attribute & ATTR_DIRECTORY) d->d_type = T_DIR;
                  else d->d_type = T_FILE;
                  
                  totlen += reclen;
                  d = (struct linux_dirent64 *)((char *)d + reclen);
             }
             
             file->_file_ptr = off; // Update file pointer
             eunlock(dp);
             printfYellow("[vfs_getdents] returning totlen=%d, new off=%u\n", totlen, off);
             return totlen;
        }
    }

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
    // ext4_dir_entry_next(&file->lwext4_dir_struct);
    // ext4_dir_entry_next(&file->lwext4_dir_struct); //< 跳过/.和/..
    d = dirp;
    while (1)
    {
        rentry = ext4_dir_entry_next(&file->lwext4_dir_struct);
        if (rentry == NULL)
            break;

        // ext4 目录项名不是以 '\0' 结尾的 C 字符串，必须使用目录项自带长度。
        const uint16_t raw_namelen = rentry->name_length;
        if (raw_namelen == 0 || raw_namelen > EXT4_DIRECTORY_FILENAME_LEN)
        {
            printfRed("[vfs_getdents] 遇到异常 ext4 目录项长度: %u\n", raw_namelen);
            return -EIO;
        }

        const uint16_t namelen = raw_namelen;
        /*
         * 长度是前四项的19加上namelen(字符串长度包括结尾的\0)
         * reclen是namelen+2,如果是+1会错误。原因是没考虑name[]开头的'\'
         */
        uint reclen = sizeof d->d_ino + sizeof d->d_off + sizeof d->d_reclen + sizeof d->d_type + namelen + 1;
        if (reclen % 8)
            reclen = reclen - reclen % 8 + 8; //<对齐
        if (reclen < sizeof(struct linux_dirent64))
            reclen = sizeof(struct linux_dirent64);

        if (totlen + (int)reclen > (int)count)
            break;

        char name[MAXPATH] = {0};
        const size_t copy_len = eastl::min<size_t>(namelen, MAXPATH - 1);
        memcpy(name, rentry->name, copy_len);
        name[copy_len] = '\0';

        // 过滤掉 O_TMPFILE 创建的临时文件，让它们在目录遍历时不可见
        if (strncmp(name, ".tmpfile_", 9) == 0)
        {
            printfYellow("vfs_getdents: filtering out O_TMPFILE: %s\n", name);
            continue; // 跳过这个条目，不返回给用户空间
        }

        memcpy(d->d_name, name, copy_len + 1);

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
    printfYellow("vfs_mkdir: creating directory: %s with mode: 0%o\n", path, mode);
    /* Check if the directory already exists */
    if (vfs_is_file_exist(path) == 1)
    {
        printfRed("vfs_mkdir: directory already exists: %s\n", path);
        return -EEXIST;
    }

    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32) {
         const char* rel_path = path;
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         
         char name_buf[FAT32_MAX_FILENAME + 1];
         struct fat32_entry *dp = enameparent((char*)rel_path, name_buf);
         if (!dp) {
             printfRed("vfs_mkdir: parent not found for %s\n", path);
             return -ENOENT;
         }
         
         elock(dp);
         struct fat32_entry *ep = ealloc(dp, name_buf, ATTR_DIRECTORY);
         eunlock(dp);
         eput(dp);
         
         if (ep) {
             eput(ep);
             printfGreen("vfs_mkdir: created FAT32 directory %s\n", path);
             return EOK;
         }
         printfRed("vfs_mkdir: failed to create FAT32 directory %s\n", path);
         return -EIO;
    }

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
    if (!is_probably_live_file_object(f))
    {
        printfRed("[vfs_fstat] 检测到异常文件对象: file=%p\n", f);
        return -EBADF;
    }

    // 检查是否是 pipe_file，如果是，直接使用其内部的 _stat
    if (f->_attrs.filetype == fs::FileTypes::FT_PIPE)
    {
        *st = f->_stat;
        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] pipe mode=0%o\n", st->mode);
        return EOK;
    }

    // 检查是否是 memfd 文件（对外名字和底层匿名临时文件路径是分离的）
    if (f->is_memfd())
    {
        // 为 memfd 文件创建合成的 stat 信息
        memset(st, 0, sizeof(fs::Kstat));

        st->dev = 0;                            // 设备号
        st->ino = f->shared_memfd_state() ? (uint64)f->shared_memfd_state() : (uint64)f;
        st->mode = S_IFREG | 0600;              // 常规文件，拥有者读写权限
        st->nlink = 1;                          // 链接数
        st->uid = 0;                            // 用户 ID (root)
        st->gid = 0;                            // 组 ID (root)
        st->rdev = 0;                           // 设备 ID
        st->size = f->memfd_size();
        st->blksize = 4096;                     // 块大小
        st->blocks = (st->size + 511) / 512;    // 512字节块数

        // 设置时间戳（使用当前时间）
        uint64 current_time = NS_to_S(TIME2NS(rdtime()));
        st->st_atime_sec = current_time;
        st->st_atime_nsec = 0;
        st->st_ctime_sec = current_time;
        st->st_ctime_nsec = 0;
        st->st_mtime_sec = current_time;
        st->st_mtime_nsec = 0;
        st->mnt_id = 0;

        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] memfd path=%s size=%u\n", f->_path_name.c_str(), st->size);
        return EOK;
    }

    // 检查是否是设备文件
    if (f->_attrs.filetype == fs::FileTypes::FT_DEVICE)
    {
        // 为设备文件创建合成的stat信息
        memset(st, 0, sizeof(fs::Kstat));
        
        st->dev = 0;                                        // 设备号
        st->ino = (uint64)f;                                // 使用文件对象地址作为伪inode号
        st->mode = S_IFCHR | 0600;                          // 字符设备，拥有者读写权限
        st->nlink = 1;                                      // 链接数
        st->uid = 0;                                        // 用户ID (root)
        st->gid = 0;                                        // 组ID (root)
        
        // 设备号设为简单的默认值，因为_dev_num是私有成员
        st->rdev = 1;                                       // 标准输出设备号
        
        st->size = 0;                                       // 设备文件大小通常为0
        st->blksize = 4096;                                 // 块大小
        st->blocks = 0;                                     // 设备文件不占用块
        
        // 设置时间戳（使用当前时间）
        uint64 current_time = NS_to_S(TIME2NS(rdtime()));
        st->st_atime_sec = current_time;
        st->st_atime_nsec = 0;
        st->st_ctime_sec = current_time;
        st->st_ctime_nsec = 0;
        st->st_mtime_sec = current_time;
        st->st_mtime_nsec = 0;
        st->mnt_id = 0;
        
        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] device path=%s rdev=%u\n", f->_path_name.c_str(), st->rdev);
        return EOK;
    }

    // 检查是否是符号链接文件
    if (f->_attrs.filetype == fs::FileTypes::FT_SYMLINK)
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

        // 获取原始 uid 和 gid
        st->uid = ext4_inode_get_uid(&inode);
        st->gid = ext4_inode_get_gid(&inode);

        st->rdev = ext4_inode_get_dev(&inode);
        st->size = ext4_inode_get_size(sb, &inode);
        st->blksize = 4096;
        st->blocks = (st->size + 511) / 512;

        st->st_atime_sec = ext4_inode_get_access_time(&inode);
        st->st_atime_nsec = (inode.atime_extra >> 2) & 0x3FFFFFFF;
        st->st_ctime_sec = ext4_inode_get_change_inode_time(&inode);
        st->st_ctime_nsec = (inode.ctime_extra >> 2) & 0x3FFFFFFF;
        st->st_mtime_sec = ext4_inode_get_modif_time(&inode);
        st->st_mtime_nsec = (inode.mtime_extra >> 2) & 0x3FFFFFFF;
        st->mnt_id = 0;

        if (Printer::trace_group_enabled())
            tracef("[vfs_fstat] symlink path=%s mode=0%o size=%u\n",
                   f->_path_name.c_str(), st->mode, st->size);
        return EOK;
    }

    struct filesystem *fs = get_fs_from_path(f->_path_name.c_str());
    if (fs && fs->type == FAT32) {
         fs::fat32_file *fat_f = (fs::fat32_file*)f;
         if (fat_f && fat_f->fat_info.entry) {
             struct fat32_entry *ep = fat_f->fat_info.entry;
             memset(st, 0, sizeof(fs::Kstat));
             
             // Use the first cluster number as a stable, non-negative inode id
             uint64 cluster_id = ep->first_clus;
             if (cluster_id == 0)
                 cluster_id = ep->off;
             if (cluster_id == 0)
                 cluster_id = ((uint64)ep) & 0x7fffffff;
             st->ino = cluster_id;
             st->dev = fs->dev;
             
             st->mode = 0;
             if (ep->attribute & ATTR_DIRECTORY) st->mode |= S_IFDIR;
             else st->mode |= S_IFREG;
             
             st->mode |= 0755;
             if (ep->attribute & ATTR_READ_ONLY) st->mode &= ~0222;
             
             st->nlink = 1;
             st->uid = 0;
             st->gid = 0;
             st->rdev = 0;
             st->size = ep->file_size;
             
             struct mntfs *mnt = (struct mntfs *)fs->fs_data;
             if (mnt) st->blksize = mnt->byts_per_clus;
             else st->blksize = 512;
             
             st->blocks = (st->size + 511) / 512;
             
             if (Printer::trace_group_enabled())
                 tracef("[vfs_fstat] fat32 path=%s mode=0%o size=%u\n",
                        f->_path_name.c_str(), st->mode, st->size);
             return EOK;
         }
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
    st->size = ext4_inode_get_size(sb, &inode);
    if (Printer::trace_group_enabled())
        tracef("[vfs_fstat] ext4 path=%s size=%u mode=0%o\n",
               f->_path_name.c_str(), st->size, st->mode);
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

int vfs_path_stat(const char *path, fs::Kstat *st, bool follow_symlinks)
{
    if (path == nullptr || st == nullptr)
    {
        return -EFAULT;
    }

    eastl::string effective_path(path);
    int length_ret = validate_linux_path_length(effective_path);
    if (length_ret != EOK)
    {
        printfRed("vfs_path_stat: path too long: len=%u\n", (uint32)effective_path.length());
        return length_ret;
    }

    if (follow_symlinks)
    {
        int resolve_ret = resolve_symlinks(effective_path, effective_path);
        if (resolve_ret < 0)
        {
            return resolve_ret;
        }
        length_ret = validate_linux_path_length(effective_path);
        if (length_ret != EOK)
        {
            printfRed("vfs_path_stat: resolved path too long: len=%u\n", (uint32)effective_path.length());
            return length_ret;
        }
    }
    else
    {
        // lstat/fstatat(AT_SYMLINK_NOFOLLOW) 只是不跟最终组件，
        // 但路径前缀里的符号链接仍然必须按正常路径遍历规则解析。
        // 否则像 ".../test_eloop/..." 这样的前缀环会直接落进底层 ext4 路径打开逻辑，
        // 本该返回 ELOOP 的场景就可能退化成异常甚至 panic。
        size_t last_slash = effective_path.find_last_of('/');
        eastl::string parent_dir;
        eastl::string filename;

        if (last_slash == eastl::string::npos)
        {
            parent_dir = ".";
            filename = effective_path;
        }
        else if (last_slash == 0)
        {
            parent_dir = "/";
            if (effective_path.length() > 1)
                filename = effective_path.substr(1);
        }
        else
        {
            parent_dir = effective_path.substr(0, last_slash);
            filename = effective_path.substr(last_slash + 1);
        }

        eastl::string resolved_parent;
        int resolve_ret = resolve_symlinks(parent_dir, resolved_parent);
        if (resolve_ret < 0)
        {
            return resolve_ret;
        }

        if (filename.empty())
        {
            effective_path = resolved_parent;
        }
        else
        {
            effective_path = resolved_parent;
            if (effective_path.empty())
                effective_path = ".";
            if (effective_path.back() != '/')
                effective_path += "/";
            effective_path += filename;
        }

        length_ret = validate_linux_path_length(effective_path);
        if (length_ret != EOK)
        {
            printfRed("vfs_path_stat: resolved path too long: len=%u\n", (uint32)effective_path.length());
            return length_ret;
        }
    }

    select_runtime_alias_path(effective_path, effective_path, false);

    int status = raw_vfs_path_stat(effective_path, st);
    if (status != EOK)
    {
        return status;
    }

    if (Printer::trace_group_enabled())
        tracef("[vfs_path_stat] path=%s mode=0%o size=%u follow_symlinks=%s\n",
               effective_path.c_str(), st->mode, st->size, follow_symlinks ? "true" : "false");

    return EOK;
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

int vfs_unlink_path(const char *path, bool remove_dir)
{
    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32)
    {
        const char *rel_path = path;
        if (strcmp(fs->path, "/") != 0)
        {
            size_t mplen = strlen(fs->path);
            if (strncmp(rel_path, fs->path, mplen) == 0)
            {
                if (rel_path[mplen] == '\0')
                    rel_path = "/";
                else if (rel_path[mplen] == '/')
                    rel_path += mplen;
            }
        }

        struct fat32_entry *ep = ename((char *)rel_path);
        if (!ep)
        {
            return -ENOENT;
        }

        if (remove_dir)
        {
            if (!(ep->attribute & ATTR_DIRECTORY))
            {
                eput(ep);
                return -ENOTDIR;
            }

            elock(ep);
            // 检查目录是否为空
            struct fat32_entry child = {};
            uint off = 0;
            int cnt = 0;
            bool nonempty = false;
            while (true)
            {
                memset(&child, 0, sizeof(child));
                int type = enext(ep, &child, off, &cnt);
                if (type == -1)
                    break;
                if (type == 1)
                {
                    nonempty = true;
                    break;
                }
                off += (cnt ? cnt : 1) << 5;
            }
            if (nonempty)
            {
                eunlock(ep);
                eput(ep);
                return -ENOTEMPTY;
            }
            eremove(ep);
            eunlock(ep);
            eput(ep);
            return 0;
        }

        if (ep->attribute & ATTR_DIRECTORY)
        {
            eput(ep);
            return -EISDIR;
        }

        if (ep->parent)
            elock(ep->parent);
        elock(ep);
        etrunc(ep);
        eremove(ep);
        eunlock(ep);
        if (ep->parent)
            eunlock(ep->parent);
        eput(ep);
        return 0;
    }

    if (remove_dir)
    {
        return vfs_ext_rmdir(path);
    }
    return vfs_ext_unlink(path);
}

int vfs_truncate(fs::file *f, size_t length)
{
    if (f == nullptr)
    {
        printfRed("vfs_truncate: file is null\n");
        return -EINVAL;
    }

    // memfd 的大小在多个 open file description 之间共享。
    // 进入真正的 ext4 截断逻辑前，先把当前 file object 的本地缓存 size
    // 与共享状态对齐，避免 /proc/self/fd 重开后的跨 fd truncate 导致旧缓存继续生效。
    f->sync_file_size_from_memfd();

    // 获取当前文件大小
    uint64_t current_size = ext4_fsize(&f->lwext4_file_struct);

    // 如果新大小等于当前大小，无需操作
    if (length == current_size)
    {
        f->_stat.size = current_size;
        f->sync_memfd_size_from_file();
        return EOK;
    }

    // 如果新大小小于当前大小，执行收缩操作
    if (length < current_size)
    {
        int status = ext4_ftruncate(&f->lwext4_file_struct, length);
        if (status != EOK)
        {
            printfRed("vfs_truncate: failed to truncate file %s to size %zu, error: %d\n",
                      f->_path_name.c_str(), length, status);
            return -status;
        }
        f->_stat.size = length;
        f->sync_memfd_size_from_file();
        return EOK;
    }

    // 如果新大小大于当前大小，需要扩展文件并零填充
    // 首先保存当前文件位置
    uint64_t original_pos = ext4_ftell(&f->lwext4_file_struct);

    // 定位到文件末尾开始零填充
    int seek_status = ext4_fseek(&f->lwext4_file_struct, current_size, SEEK_SET);
    if (seek_status != EOK)
    {
        printfRed("vfs_truncate: failed to seek to position %llu in file %s, error: %d\n",
                  current_size, f->_path_name.c_str(), seek_status);
        return -seek_status;
    }

    // 计算需要零填充的字节数
    size_t zero_fill_size = length - current_size;

    // 分块写入零数据，避免一次性分配过大的缓冲区
    const size_t chunk_size = 4096; // 4KB chunks
    char zero_buffer[chunk_size];
    memset(zero_buffer, 0, chunk_size);

    size_t bytes_written_total = 0;
    while (bytes_written_total < zero_fill_size)
    {
        size_t bytes_to_write = eastl::min(chunk_size, zero_fill_size - bytes_written_total);
        size_t bytes_written = 0;

        int write_status = ext4_fwrite(&f->lwext4_file_struct, zero_buffer, bytes_to_write, &bytes_written);
        if (write_status != EOK)
        {
            printfRed("vfs_truncate: failed to write zeros during file extension for %s, error: %d\n",
                      f->_path_name.c_str(), write_status);
            // 尝试恢复原始文件位置
            ext4_fseek(&f->lwext4_file_struct, original_pos, SEEK_SET);
            return -write_status;
        }

        if (bytes_written == 0)
        {
            printfRed("vfs_truncate: no bytes written during zero-fill for file %s\n", f->_path_name.c_str());
            ext4_fseek(&f->lwext4_file_struct, original_pos, SEEK_SET);
            return -EIO;
        }

        bytes_written_total += bytes_written;
    }

    // 恢复原始文件位置（如果原始位置仍在有效范围内）
    if (original_pos <= length)
    {
        ext4_fseek(&f->lwext4_file_struct, original_pos, SEEK_SET);
    }
    else
    {
        // 如果原始位置超出新文件大小，设置到文件末尾
        ext4_fseek(&f->lwext4_file_struct, length, SEEK_SET);
    }

    // 更新文件大小
    f->_stat.size = length;
    f->sync_memfd_size_from_file();

    printfGreen("vfs_truncate: successfully extended file %s from %llu to %zu bytes with zero-fill\n",
                f->_path_name.c_str(), current_size, length);

    return EOK;
}
int vfs_chmod(eastl::string pathname, mode_t mode)
{
    if (vfs_is_readonly_path(pathname))
    {
        return -EROFS;
    }

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

    // 与 vfs_truncate 同理，memfd 进入 size-sensitive 逻辑前先同步共享大小。
    f->sync_file_size_from_memfd();

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
    int status = vfs_truncate(f, target_size);
    if (status != EOK)
    {
        printfRed("vfs_fallocate: failed to allocate space for file %s, error: %d\n",
                  f->_path_name.c_str(), status);
        return status;
    }

    // 更新文件大小信息
    f->_stat.size = target_size;
    f->sync_memfd_size_from_file();

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

int vfs_chown(const eastl::string &pathname, int owner, int group, bool follow_symlinks)
{
    if (vfs_is_readonly_path(pathname))
    {
        return -EROFS;
    }

    // Basic existence check
    if (vfs_is_file_exist(pathname.c_str()) != 1)
    {
        return -ENOENT;
    }

    // Symlink handling: if not following, ensure the path is a symlink and operate on it directly.
    // Our ext4_owner_set operates on the path's inode. vfs_openat already has a resolver,
    // here we emulate lchown by avoiding resolving final symlink.
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        // Resolve symlinks to the final target
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
        {
            target_path = resolved;
        }
        else if (r < 0)
        {
            return r; // e.g., -ELOOP
        }
    }

    // Handle "-1 means unchanged" per POSIX: read current owner/group if needed
    uint32_t cur_uid = 0, cur_gid = 0;
    int gstat = ext4_owner_get(target_path.c_str(), &cur_uid, &cur_gid);
    if (gstat != EOK)
    {
        // If we can't read current owner/group, propagate as access error
        return -EACCES;
    }

    uint32_t new_uid = (owner < 0) ? cur_uid : (uint32_t)owner;
    uint32_t new_gid = (group < 0) ? cur_gid : (uint32_t)group;

    int status = ext4_owner_set(target_path.c_str(), new_uid, new_gid);
    if (status != EOK)
    {
        // Map typical errors
        if (status == ENOENT)
            return -ENOENT;
        if (status == EROFS)
            return -EROFS;
        return -EACCES;
    }
    return 0;
}

int vfs_owner_get(const eastl::string &pathname, uint32_t &uid, uint32_t &gid, bool follow_symlinks)
{
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;

    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    uint32_t u = 0, g = 0;
    int s = ext4_owner_get(target_path.c_str(), &u, &g);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        return -EACCES;
    }
    uid = u;
    gid = g;
    return 0;
}

int vfs_mode_get(const eastl::string &pathname, uint32_t &mode, bool follow_symlinks)
{
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    uint32_t m = 0;
    int s = ext4_mode_get(target_path.c_str(), &m);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        return -EACCES;
    }
    mode = m;
    return 0;
}

int vfs_mode_set(const eastl::string &pathname, uint32_t mode, bool follow_symlinks)
{
    if (vfs_is_readonly_path(pathname))
        return -EROFS;
    if (vfs_is_file_exist(pathname.c_str()) != 1)
        return -ENOENT;
    eastl::string target_path = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r == 0 && !resolved.empty())
            target_path = resolved;
        else if (r < 0)
            return r;
    }
    int s = ext4_mode_set(target_path.c_str(), mode);
    if (s != EOK)
    {
        if (s == ENOENT)
            return -ENOENT;
        if (s == EROFS)
            return -EROFS;
        return -EACCES;
    }
    return 0;
}

int vfs_register_mount(const eastl::string &mount_path, bool read_only)
{
    if (mount_path.empty())
        return -EINVAL;

    eastl::string normalized_path = normalize_path(mount_path);
    if (normalized_path.empty())
        normalized_path = "/";

    for (auto &entry : g_mount_overrides)
    {
        if (entry.path == normalized_path)
        {
            entry.read_only = read_only;
            return 0;
        }
    }

    MountOverride entry;
    entry.path = normalized_path;
    entry.read_only = read_only;
    g_mount_overrides.push_back(entry);
    return 0;
}

int vfs_unregister_mount(const eastl::string &mount_path)
{
    if (mount_path.empty())
        return -EINVAL;

    eastl::string normalized_path = normalize_path(mount_path);
    if (normalized_path.empty())
        normalized_path = "/";

    for (auto it = g_mount_overrides.begin(); it != g_mount_overrides.end(); ++it)
    {
        if (it->path == normalized_path)
        {
            g_mount_overrides.erase(it);
            return 0;
        }
    }

    return -ENOENT;
}

bool vfs_is_readonly_path(const eastl::string &path)
{
    if (path.empty())
        return false;

    eastl::string normalized_path = normalize_path(path);
    if (normalized_path.empty())
        normalized_path = "/";

    const MountOverride *entry = find_mount_override(normalized_path);
    return entry != nullptr && entry->read_only;
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
    return true; // 其他情况下有冲突
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

int vfs_write_file(const char *path, uint64 buffer_addr, size_t offset, size_t size)
{
    // 检查文件是否存在
    if (vfs_is_file_exist(path) != 1)
    {
        printfRed("File does not exist: %s\n", path);
        return -ENOENT;
    }

    struct filesystem *fs = get_fs_from_path(path);
    if (fs && fs->type == FAT32) {
         const char* rel_path = path;
         if (strcmp(fs->path, "/") != 0) {
             size_t mplen = strlen(fs->path);
             if (strncmp(rel_path, fs->path, mplen) == 0) {
                 if (rel_path[mplen] == '\0') rel_path = "/";
                 else if (rel_path[mplen] == '/') rel_path += mplen;
             }
         }
         struct fat32_entry *ep = ename((char*)rel_path);
         if (!ep) return -ENOENT;
         
         elock(ep);
         int n = ewrite(ep, 0, buffer_addr, offset, size);
         eunlock(ep);
         eput(ep);
         return n;
    }

    int res;
    ext4_file file;

    // 打开文件（读写模式）
    res = ext4_fopen(&file, path, "r+b");
    if (res != EOK)
    {
        printfRed("Failed to open file for writing: %d\n", res);
        return -res;
    }

    // 如果有偏移，设置文件指针位置
    if (offset > 0)
    {
        res = ext4_fseek(&file, offset, SEEK_SET);
        if (res != EOK)
        {
            printfRed("Failed to seek file: %d\n", res);
            ext4_fclose(&file);
            return -res;
        }
    }

    // 写入数据
    size_t bytes_written;
    res = ext4_fwrite(&file, (void *)buffer_addr, size, &bytes_written);
    if (res != EOK)
    {
        printfRed("Failed to write file: %d\n", res);
        ext4_fclose(&file);
        return -res;
    }

    // 关闭文件
    res = ext4_fclose(&file);
    if (res != EOK)
    {
        printfRed("Failed to close file: %d\n", res);
        return -res;
    }

    // 返回实际写入的字节数
    return bytes_written;
}

// ====================== xattr VFS wrappers ==========================

static inline int map_ext4_err_to_sys(int r)
{
    // ext4.cc returns EOK on success and standard errno values otherwise.
    if (r == EOK)
        return 0;
    // Already errno-like, return negative for syscall path callers
    return -r;
}

int vfs_setxattr(const eastl::string &pathname, const char *name, const void *data, size_t size, bool follow_symlinks)
{
    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    int r = ext4_setxattr(target.c_str(), name, strlen(name), data, size);
    return map_ext4_err_to_sys(r);
}

int vfs_getxattr(const eastl::string &pathname, const char *name, void *buf, size_t buf_size, size_t &out_size, bool follow_symlinks)
{
    out_size = 0;
    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    size_t data_size = 0;
    int r = ext4_getxattr(target.c_str(), name, strlen(name), buf, buf_size, &data_size);
    if (r == EOK)
    {
        out_size = data_size;
    }
    return map_ext4_err_to_sys(r);
}

int vfs_listxattr(const eastl::string &pathname, char *list, size_t size, size_t &ret_size, bool follow_symlinks)
{
    ret_size = 0;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    size_t used = 0;
    int r = ext4_listxattr(target.c_str(), list, size, &used);
    if (r == EOK)
    {
        ret_size = used;
    }
    return map_ext4_err_to_sys(r);
}

int vfs_removexattr(const eastl::string &pathname, const char *name, bool follow_symlinks)
{
    if (!name)
        return -EINVAL;
    if (pathname.empty())
        return -EINVAL;
    eastl::string target = pathname;
    if (follow_symlinks)
    {
        eastl::string resolved;
        int r = resolve_symlinks(pathname, resolved);
        if (r < 0)
            return r;
        if (!resolved.empty())
            target = resolved;
    }
    int r = ext4_removexattr(target.c_str(), name, strlen(name));
    return map_ext4_err_to_sys(r);
}

int vfs_fsetxattr(fs::file *f, const char *name, const void *data, size_t size)
{
    if (!f || !name)
    {
        printfRed("vfs_fsetxattr: invalid file or name\n");
        return -EINVAL;
    }
    return vfs_setxattr(f->_path_name, name, data, size, /*follow_symlinks*/ true);
}

int vfs_fgetxattr(fs::file *f, const char *name, void *buf, size_t buf_size, size_t &out_size)
{
    if (!f || !name)
        return -EINVAL;
    return vfs_getxattr(f->_path_name, name, buf, buf_size, out_size, /*follow_symlinks*/ true);
}

int vfs_flistxattr(fs::file *f, char *list, size_t size, size_t &ret_size)
{
    if (!f)
        return -EINVAL;
    return vfs_listxattr(f->_path_name, list, size, ret_size, /*follow_symlinks*/ true);
}

int vfs_fremovexattr(fs::file *f, const char *name)
{
    if (!f || !name)
        return -EINVAL;
    return vfs_removexattr(f->_path_name, name, /*follow_symlinks*/ true);
}

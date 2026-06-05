#include "fs/vfs/ops.hh"

#include <fs/fcntl.hh>
#include "proc_manager.hh"
#include "fs/vfs/file.hh"
#include "fs/vfs/fs.hh"

#include "libs/string.hh"
#include "vfs_ext4_ext.hh"
#include "fs/lwext4/ext4_errno.hh"
#include <EASTL/vector.h>


struct inode *find_inode(char *path, int dirfd, char *name) {
    // struct inode *ip;
    // struct proc *p = myproc();
    //
    // //绝对路径 || 相对路径
    // if (*path == '/' || dirfd == AT_FDCWD) {
    //     ip = (name == NULL) ? namei(path) : nameiparent(path, name);
    //     if (ip == 0) {
    //         return 0;
    //     }
    //     return ip;
    // }
    // struct file *f;
    // if (dirfd < 0 || dirfd >= NOFILE || (f=p->ofile[dirfd]) == 0) {
    //     return 0;
    // }
    // struct inode *oldcwd = p->cwd;
    // p->cwd = f->f_ip;
    // ip = (name == NULL) ? namei(path) : nameiparent(path, name);
    // p->cwd = oldcwd;
    // if (ip == 0) {
    //     return 0;
    // }
    // return ip;
    return NULL;
}

namespace
{
    eastl::string normalize_absolute_path(const eastl::string &input)
    {
        eastl::vector<eastl::string> components;
        size_t i = 0;

        while (i < input.size())
        {
            while (i < input.size() && input[i] == '/')
            {
                ++i;
            }

            size_t start = i;
            while (i < input.size() && input[i] != '/')
            {
                ++i;
            }

            if (start == i)
            {
                continue;
            }

            eastl::string component = input.substr(start, i - start);
            if (component == ".")
            {
                continue;
            }

            if (component == "..")
            {
                if (!components.empty())
                {
                    components.pop_back();
                }
                continue;
            }

            components.push_back(component);
        }

        eastl::string normalized = "/";
        for (size_t idx = 0; idx < components.size(); ++idx)
        {
            if (idx != 0)
            {
                normalized += "/";
            }
            normalized += components[idx];
        }

        return normalized;
    }
}

eastl::string get_absolute_path(const char *path, const char *cwd)
{
    const char *safe_path = path != nullptr ? path : "";
    const char *safe_cwd = (cwd != nullptr && cwd[0] != '\0') ? cwd : "/";

    eastl::string raw_path;
    if (safe_path[0] == '\0')
    {
        raw_path = safe_cwd;
    }
    else if (safe_path[0] == '/')
    {
        raw_path = safe_path;
    }
    else
    {
        raw_path = safe_cwd;
        if (raw_path.empty())
        {
            raw_path = "/";
        }
        if (raw_path.back() != '/')
        {
            raw_path += "/";
        }
        raw_path += safe_path;
    }

    if (raw_path.empty() || raw_path[0] != '/')
    {
        eastl::string with_root = "/";
        with_root += raw_path;
        raw_path = with_root;
    }

    return normalize_absolute_path(raw_path);
}

int get_absolute_path(const char *path, const char *cwd, char *absolute_path, size_t absolute_path_size)
{
    if (absolute_path == nullptr || absolute_path_size == 0)
    {
        return -EINVAL;
    }

    eastl::string normalized = get_absolute_path(path, cwd);
    if (normalized.size() + 1 > absolute_path_size)
    {
        absolute_path[0] = '\0';
        return -ENAMETOOLONG;
    }

    strcpy(absolute_path, normalized.c_str());
    return EOK;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
char*
skipelem(char *path, char *name)
{
    char *s;
    int len;

    while(*path == '/')
        path++;
    if(*path == 0)
        return 0;
    s = path;
    while(*path != '/' && *path != 0)
        path++;
    len = path - s;
    if(len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while(*path == '/')
        path++;
    return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
struct inode*
namex(char *path, int nameiparent, char *name)
{
    // struct inode *ip, *next;
    //
    // // if(*path == '/') {
    // //
    // // }
    // // else
    // //     ip = idup(myproc()->cwd);
    //
    // while((path = skipelem(path, name)) != 0){
    //     ilock(ip);
    //     if(ip->i_type != T_DIR){
    //         iunlockput(ip);
    //         return 0;
    //     }
    //     if(nameiparent && *path == '\0'){
    //         // Stop one level early.
    //         iunlock(ip);
    //         return ip;
    //     }
    //     if((next = dirlookup(ip, name, 0)) == 0){
    //         iunlockput(ip);
    //         return 0;
    //     }
    //     iunlockput(ip);
    //     ip = next;
    // }
    // if(nameiparent){
    //     iput(ip);
    //     return 0;
    // }
    // return ip;
    return NULL;
}

struct inode*
namei(char *path)
{
    char name[EXT4_PATH_LONG_MAX];
    int path_ret = get_absolute_path(path, proc::k_pm.get_cur_pcb()->_cwd_name.c_str(), name, sizeof(name));
    if (path_ret < 0)
    {
        return NULL;
    }
    // printf("%s %s\n", name, myproc()->cwd.path);
    return vfs_ext_namei(name);
}

struct inode*
nameiparent(char *path, char *name)
{
    return namex(path, 1, name);
}

#include "fs/vfs/ops.hh"

#include <fs/fcntl.hh>
#include "proc_manager.hh"
#include "fs/vfs/file.hh"
#include "fs/vfs/fs.hh"

#include "libs/string.hh"
#include "vfs_ext4_ext.hh"


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

void get_absolute_path(const char *path, const char *cwd, char *absolute_path) {
     // 强烈建议：函数应该接受 absolute_path 的大小作为参数，例如:
    // void get_absolute_path(const char *path, const char *cwd, char *absolute_path, size_t max_len)

    /* 为空，工作目录 */
    if (path == NULL || path[0] == '\0') // 也应处理空字符串的path
    {
        // 警告：strlen(cwd) 可能大于 absolute_path 的容量
        // strncpy(absolute_path, cwd, strlen(cwd)); // 这行是有问题的：
                                                    // 1. 如果 strlen(cwd) == 第三个参数，不保证空终止
                                                    // 2. 第三个参数应该是缓冲区大小减1，然后手动空终止
        // 应该使用 snprintf(absolute_path, max_len, "%s", cwd);
        // 暂且假设 buffer 够大，并使用 strcpy，但这不安全
        strcpy(absolute_path, cwd);
    }
    /* '/'开头为绝对路径 */
    else if (path[0] == '/')
    {
        // 警告：strlen(path) 可能大于 absolute_path 的容量
        strcpy(absolute_path, path);
    }
    /* 相对路径，拼接成绝对路径 */
    else
    {
        // 警告：组合路径可能溢出
        // 应该使用 snprintf(absolute_path, max_len, "%s/%s", cwd, path);
        // 注意：如果 cwd 是 "/"，这会产生 "//path"。后续逻辑会处理多余的'/'
        strcpy(absolute_path, cwd);
        // 如果 cwd 本身就是根目录 "/"，避免产生 "//"
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
             strcat(absolute_path, "/");
        }
        strcat(absolute_path, path);
    }

    /* 处理 ./ and ../ */
    // 这一段逻辑非常复杂，且容易出错。
    // 核心思想是使用一个读写指针在原地修改路径字符串。
    // 更好的方法通常是先将路径分割成组件，处理组件列表，然后重新组合。
    char *p = absolute_path;
    while (*p != '\0')
    {
        /* ./ */
        if (*p == '.' && *(p + 1) == '/')
        {
            // strcpy 可能有风险，如果 p+2 指向的字符串很长。
            // 但这里是原地删除，通常是安全的，只要原 absolute_path 有效。
            strcpy(p, p + 2); // 将 "foo./bar" 变为 "foobar" (p 指向 . 的位置)
                              // p 会保持在原地，下一轮循环会检查新的 *p
        }
        /* ../ */
        else if (*p == '.' && *(p + 1) == '.' && *(p + 2) == '/')
        {
            char *current_dotdot_ptr = p; // 指向 '../' 中的第一个 '.'
            char *q = p - 2; // q 指向 '../' 前一个字符，例如 "a/b/../c"，q 指向 'b'

            if (q < absolute_path) { // 情况1: 路径以 "../" 或 "/../" 开头
                if (absolute_path[0] == '/') { // 例如 "/../foo"
                    // 目标是变成 "/foo"
                    // current_dotdot_ptr + 2 是 '/'，current_dotdot_ptr + 3 是 "foo" 的 'f'
                    // 将 "foo" 移动到 absolute_path + 1 的位置
                    memmove(absolute_path + 1, current_dotdot_ptr + 3, strlen(current_dotdot_ptr + 3) + 1);
                    p = absolute_path; // 从头开始重新检查，因为路径已改变
                } else { // 例如 "../foo" (相对路径)
                    // 目标是变成 "foo"
                    strcpy(absolute_path, current_dotdot_ptr + 3);
                    p = absolute_path -1; // p会++，所以从新串头部开始，这里设为absolute_path前一个位置
                }
            }
            else // q >= absolute_path (正常的 "segment/../" 情况)
            {
                // q 指向 segment最后一个字符（如 'b' in /a/b/..），或者 segment 前的 '/' (如果 segment 为空，如 //../)
                // 我们需要找到 q 指向的 segment 之前的那个 '/'
                while (q > absolute_path && *(q - 1) != '/') { // 注意是 *(q-1)
                    q--;
                }
                // 此刻 q 指向 segment 的第一个字符 (如 'b')，或者如果路径是 /../ q会是absolute_path+1
                // 如果 q == absolute_path 且 *q != '/', 说明是类似 "abc/../def" 的情况，q指向'a'
                // 我们的目标是将 current_dotdot_ptr + 3 (即 "../" 后面的部分) 拷贝到 q 的位置
                // 例如： /a/b/../c -> p 指向 '.', q 最终指向 'b'
                // strcpy(q, current_dotdot_ptr + 3) -> 将 "c" 拷贝到 "b" 的位置
                // 结果: /a/c
                // 修正查找 q 的逻辑：q 应该指向上一级目录的起始位置或者根目录后的第一个字符
                char *parent_dir_start = p - 1; // 指向 "../" 前的 '/'
                if (parent_dir_start == absolute_path && *parent_dir_start == '/') { // 路径是 "/../..."
                     memmove(absolute_path + 1, current_dotdot_ptr + 3, strlen(current_dotdot_ptr + 3) + 1);
                     p = absolute_path; // 从 /foo 的 / 开始
                     continue; // 跳过 p++
                } else if (parent_dir_start < absolute_path) { // 路径是 "../..." (相对)
                    // 无法再往上，保持原样，跳过 ".."
                     p = current_dotdot_ptr + 2; // p 指向 '/'
                } else {
                    // parent_dir_start 指向 "dir/../" 中的 '/'
                    // 我们需要找到这个 '/' 之前的 segment 的起始
                    char *segment_to_remove_start = parent_dir_start;
                    while(segment_to_remove_start > absolute_path && *(segment_to_remove_start - 1) != '/') {
                        segment_to_remove_start--;
                    }
                    // segment_to_remove_start 现在指向要移除的目录名的第一个字符 (例如 /abc/def/../ghi, 指向 d)
                    // 或者如果absolute_path是 "/def/../ghi", segment_to_remove_start 指向 d (absolute_path+1)

                    strcpy(segment_to_remove_start, current_dotdot_ptr + 3);
                    p = segment_to_remove_start -1; // 让 p 在下一次循环指向新内容开始的地方或之前
                                                    // (p会自增，所以减1)
                }
            }
        }
        else
        {
            p++;
        }
    }

    /* 处理尾巴的 . 和 .. */
    // 这一段非常复杂且容易出错，特别是对于短路径。
    // 一个更健壮的规范化循环通常能处理这些情况，无需单独的尾部处理。
    // 例如，"foo/." 或 "foo/.." 应该在主循环中作为组件被处理。
    // 当前这个实现可能无法正确处理所有情况，如 "a/b/." (未被主循环处理) 或 "a/b/.."
    // 而且指针运算 `absolute_path + strlen(absolute_path) - 2` 等对于长度小于2或3的字符串是危险的。

    // 建议：移除这个复杂的尾部处理，并增强主规范化循环，
    // 使其能识别路径末尾的 "." 和 ".." 组件。
    // 如果保留，需要极仔细地测试各种边界条件。
    // (由于其复杂性和潜在问题，这里暂时不修改这个特定块，但强烈建议重新评估)
    // ---- START OF SUSPECT TRAILING CODE ----
    size_t len_before_trailing_handle = strlen(absolute_path);
    if (len_before_trailing_handle >= 2) { // 基本的长度保护
        char *pt = absolute_path + len_before_trailing_handle - 1; // 指向最后一个字符

        // 处理 path/. -> path
        if (*pt == '.' && len_before_trailing_handle >= 2 && *(pt - 1) == '/') {
            *(pt - 1) = '\0';
        } 
        // 处理 path/.. -> (上一级目录)
        // 这个逻辑应该由主循环的 ".." 处理更稳健地完成，当 ".." 是最后一个组件时。
        // 现有主循环只处理 "../<something>"，不处理 "..[EOS]"
        // 此处简化：如果路径以 "/.." 结尾，例如 "foo/bar/..", 结果应为 "foo"
        else if (*pt == '.' && len_before_trailing_handle >= 3 && *(pt - 1) == '.' && *(pt - 2) == '/') {
            char *slash_before_dotdot = pt - 2; // 指向 /.. 中的 /
            if (slash_before_dotdot == absolute_path) { //  路径是 "/.."
                absolute_path[1] = '\0'; // 变成 "/"
            } else {
                char *q = slash_before_dotdot - 1; // 指向 /.. 前一个字符
                while (q > absolute_path && *(q - 1) != '/') {
                    q--;
                }
                *q = '\0'; // 从上一级目录的起始处截断
            }
        }
    } else if (len_before_trailing_handle == 1 && absolute_path[0] == '.') { // 路径是 "."
        absolute_path[0] = '\0'; // 变成空，后续会处理成 "/"
    }
    // 对于 ".." 的情况，如果主循环能处理末尾的 ".."，这里就不需要了。
    // ---- END OF SUSPECT TRAILING CODE ----
    

    /* 移除开头的多余斜杠，例如 ///foo -> /foo */
    if (absolute_path[0] == '/') {
        char *first_char = absolute_path;
        while (*(first_char) == '/' && *(first_char + 1) == '/') {
            strcpy(first_char, first_char + 1);
        }
    }

    /* 移除尾部斜杠, 但保留根目录 "/" */
    size_t len = strlen(absolute_path);
    if (len > 1 && absolute_path[len - 1] == '/')
    {
        absolute_path[len - 1] = '\0';
        --len; // 更新长度 (虽然在这个函数后面没用到新len)
    }

    /* 如果路径为空 (例如, ".." 在根目录下处理后, 或者 "." 处理后), 设为 "/" */
    if (strlen(absolute_path) == 0)
    {
        strcpy(absolute_path, "/"); // 警告: buffer 必须至少有2字节
    }
    /* 如果处理结果不是以'/'开头 (这通常不应该发生，如果cwd是绝对路径且逻辑正确) */
    /* 这可能意味着 cwd 本身不是绝对路径，或者之前的逻辑错误地移除了开头的 '/' */
    else if (absolute_path[0] != '/')
    {
        // 这段逻辑非常可疑。如果cwd是绝对路径，结果也应该是绝对路径。
        // 如果cwd是相对路径，那么这个函数就不能保证得到绝对路径。
        // 假设目标是确保结果总是以'/'开头，即使输入不规范。

        size_t len2 = strlen(absolute_path);
        // 警告：需要检查 absolute_path 缓冲区是否有空间再加一个字符
        char *x = absolute_path + len2; // 指向末尾的 '\0'
        *(x + 1) = '\0'; // 新的末尾
        while (x > absolute_path)
        {
            *x = *(x - 1); // 向后移动字符
            x--;
        }
        *x = '/'; // 在开头插入 '/'
    }
}

eastl::string get_absolute_path(const char *path, const char *cwd)
{
    // panic("not implemented yet");
    char temp_path[EXT4_PATH_LONG_MAX];
    get_absolute_path(path, cwd, temp_path);
    eastl::string absolute_path(temp_path);
    return absolute_path;
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
    get_absolute_path(path, proc::k_pm.get_cur_pcb()->_cwd_name.c_str(), name);
    // printf("%s %s\n", name, myproc()->cwd.path);
    return vfs_ext_namei(name);
}

struct inode*
nameiparent(char *path, char *name)
{
    return namex(path, 1, name);
}

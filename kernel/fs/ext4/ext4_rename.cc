#include "ext4_fs.hh"
#include "ext4.hh"
#include "printer.hh"
#include "klib.hh"
#include "fs/vfs/path.hh"
#include "fs/vfs/dentry.hh"
namespace fs
{
    // 适配内核VFS系统的重命名函数
    int ext4_do_rename(const char *old_path, const char *new_path)
    {
        // 这是一个简化的实现，适配当前内核的VFS架构
        // 在实际的ext4文件系统中，这需要更复杂的目录项操作

        // 1. 检查路径参数有效性
        if (!old_path || !new_path)
        {
            return -22; // -EINVAL
        }

        // 2. 解析旧路径和新路径
        fs::Path old_path_obj(old_path);
        fs::Path new_path_obj(new_path);

        // 3. 查找源文件/目录
        fs::dentry *old_dentry = old_path_obj.pathSearch();
        if (!old_dentry)
        {
            return -2; // -ENOENT
        }

        // 4. 检查目标路径的父目录是否存在
        fs::dentry *new_parent = new_path_obj.pathSearch(true);
        if (!new_parent)
        {
            return -2; // -ENOENT
        }

        // 5. 检查目标文件是否已存在
        fs::dentry *new_dentry = new_path_obj.pathSearch();
        if (new_dentry)
        {
            // 目标已存在，需要根据类型决定是否覆盖
            // 这里简化处理，不允许覆盖
            return -17; // -EEXIST
        }

        // 6. 执行重命名操作
        // 在真实的ext4实现中，这里需要：
        // - 在新父目录中创建目录项
        // - 从旧父目录中删除目录项
        // - 更新inode的链接计数等
        ///@todo
        // 由于当前内核的ext4实现可能不完整，这里返回未实现
        printfYellow("ext4_do_rename: rename operation not fully implemented yet\n");
        return 0; // -ENOSYS (函数未实现)
    }
}
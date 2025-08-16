#ifndef VFS_UTILS_HH
#define VFS_UTILS_HH

#include <EASTL/string.h>
#include "fs/vfs/ops.hh"
#include "fs/vfs/file/file.hh"

// 将flags转换为可读的字符串表示
eastl::string flags_to_string(uint flags);

// 路径规范化函数：处理 . 和 ..
eastl::string normalize_path(const eastl::string &path);

int vfs_openat(eastl::string absolute_path, fs::file* &file, uint flags, int mode);
int vfs_is_dir(eastl::string &absolute_path);
int vfs_path2filetype(eastl::string &absolute_path);
int create_and_write_file(const char *path, const char *data);
int vfs_is_file_exist(const char *path);
uint vfs_read_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_write_file(const char *path, uint64 buffer_addr, size_t offset, size_t size);
int vfs_fstat(fs::file *f, fs::Kstat *st);
int vfs_path_stat(const char *path, fs::Kstat *st, bool follow_symlinks = true);
int vfs_getdents(fs::file *const file, struct linux_dirent64 *dirp, uint count);
int vfs_mkdir(const char *path, uint64_t mode);
int vfs_frename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_truncate(fs::file *f, size_t length);
int vfs_chmod(eastl::string pathname, mode_t mode);
// Change owner/group for a path. If follow_symlinks is true, operate on the target of symlink
// else operate on the link itself (lchown-like). Returns 0 on success, negative errno on error.
int vfs_chown(const eastl::string &pathname, int owner, int group, bool follow_symlinks);
// Get owner/group for a path. follow_symlinks controls final symlink resolution.
int vfs_owner_get(const eastl::string &pathname, uint32_t &uid, uint32_t &gid, bool follow_symlinks);
// Get file mode for a path. follow_symlinks controls final symlink resolution.
int vfs_mode_get(const eastl::string &pathname, uint32_t &mode, bool follow_symlinks);
// Set file mode for a path. follow_symlinks controls final symlink resolution.
int vfs_mode_set(const eastl::string &pathname, uint32_t mode, bool follow_symlinks);
int vfs_fallocate(fs::file *f,off_t offset, size_t length);
int vfs_free_file(struct fs::file *file);
int vfs_copy_file_range(int f_in,off_t offset_in, int f_out,off_t offset_out,size_t size,uint flags);
bool is_lock_conflict(const struct flock &existing_lock, const struct flock &new_lock);
bool check_file_lock_access(const struct flock &file_lock, off_t offset, size_t size, bool is_write);

// Extended attributes (xattr) interfaces for VFS
// Path-based; follow_symlinks controls final symlink resolution (true for setxattr/getxattr/listxattr/removexattr, false for l* variants)
int vfs_setxattr(const eastl::string &pathname, const char *name, const void *data, size_t size, bool follow_symlinks);
// buf may be nullptr with buf_size==0 to query size; out_size returns actual size on success
int vfs_getxattr(const eastl::string &pathname, const char *name, void *buf, size_t buf_size, size_t &out_size, bool follow_symlinks);
// list may be nullptr with size==0 to query required size; ret_size returns used bytes
int vfs_listxattr(const eastl::string &pathname, char *list, size_t size, size_t &ret_size, bool follow_symlinks);
int vfs_removexattr(const eastl::string &pathname, const char *name, bool follow_symlinks);

// FD-based helpers implemented via file->_path_name
int vfs_fsetxattr(fs::file *f, const char *name, const void *data, size_t size);
int vfs_fgetxattr(fs::file *f, const char *name, void *buf, size_t buf_size, size_t &out_size);
int vfs_flistxattr(fs::file *f, char *list, size_t size, size_t &ret_size);
int vfs_fremovexattr(fs::file *f, const char *name);

#endif // VFS_UTILS_HH